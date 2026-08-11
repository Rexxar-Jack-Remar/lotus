#include "Checker/KINT/MKintPass.h"

#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>

#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <z3++.h>

using namespace llvm;

namespace kint {

MKintPass::MKintPass()
    : m_solver(std::nullopt), m_function_timeout(FunctionTimeout),
      m_path_limit(MaxPathsPerFunction) {
  m_taint_analysis = std::make_unique<TaintAnalysis>();
  m_bug_detection = std::make_unique<BugDetection>();

  // Register bug types with BugReportMgr (shared pattern)
  BugReportMgr &mgr = BugReportMgr::get_instance();
  m_intOverflowTypeId =
      mgr.register_bug_type("Integer Overflow", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-190");
  m_divByZeroTypeId =
      mgr.register_bug_type("Divide by Zero", BugDescription::BI_MEDIUM,
                            BugDescription::BC_ERROR, "CWE-369");
  m_badShiftTypeId =
      mgr.register_bug_type("Bad Shift", BugDescription::BI_MEDIUM,
                            BugDescription::BC_ERROR, "Invalid shift amount");
  m_arrayOOBTypeId =
      mgr.register_bug_type("Array Out of Bounds", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-119, CWE-125");
  m_deadBranchTypeId =
      mgr.register_bug_type("Dead Branch", BugDescription::BI_LOW,
                            BugDescription::BC_ERROR, "Unreachable code");
}

void MKintPass::backedge_analysis(const Function &F) {
  // Compute true loop backedges using a DFS-based algorithm.
  // An edge pred->succ is a backedge iff succ dominates pred (i.e., succ is
  // an ancestor of pred in the DFS spanning tree).
  // We store, for each block B, the set of predecessors P such that P->B is
  // a backedge.  The existing usage is: m_backedges[cur].contains(pred).

  // Initialize all entries so every block has an (empty) set.
  for (const auto &bb_ref : F) {
    const auto *bb = &bb_ref;
    if (m_backedges.count(bb) == 0)
      m_backedges[bb] = {};
  }

  // DFS colouring: 0 = white (unvisited), 1 = grey (on stack), 2 = black
  // (done).
  DenseMap<const BasicBlock *, int> color;
  std::vector<std::pair<const BasicBlock *, bool>> stack; // (block, entered)
  stack.push_back(std::make_pair(&F.getEntryBlock(), false));

  while (!stack.empty()) {
    const BasicBlock *bb = stack.back().first;
    const bool leaving = stack.back().second;
    stack.pop_back();

    if (leaving) {
      color[bb] = 2; // black
      continue;
    }

    if (color[bb] == 1)
      continue; // already on stack (cycle detected earlier)
    if (color[bb] == 2)
      continue; // already fully processed

    color[bb] = 1;                             // grey: on the DFS stack
    stack.push_back(std::make_pair(bb, true)); // push "leaving" marker

    for (const auto *succ : successors(bb)) {
      if (color[succ] == 1) {
        // succ is an ancestor in the DFS tree -> bb->succ is a backedge.
        m_backedges[succ].insert(bb);
      } else if (color[succ] == 0) {
        stack.push_back(std::make_pair(succ, false));
      }
    }
  }
}

PreservedAnalyses MKintPass::run(Module &M, ModuleAnalysisManager &MAM) {
  MKINT_LOG() << "Running MKint pass on module " << M.getName();

  // Refresh performance options (they may change across runs).
  m_function_timeout = FunctionTimeout;
  m_path_limit = MaxPathsPerFunction;
  m_robust_reachability = RobustReachability;
  m_dump_ef_path = DumpEFConstraints;
  m_robust_universal_unknown_loads = RobustUniversalUnknownLoads;
  m_robust_universal_external_globals = RobustUniversalExternalGlobals;
  m_robust_universal_inline_asm = RobustUniversalInlineAsm;
  m_summary_timeout = SummaryTimeout;
  m_summary_path_limit = SummaryMaxPathsPerFunction;
  parseRobustBugFilter(RobustChecks);

  // Print checker configuration
  MKINT_LOG() << "Checker Configuration:";
  MKINT_LOG() << "  Integer Overflow: "
              << (CheckIntOverflow ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Division by Zero: "
              << (CheckDivByZero ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Bad Shift: " << (CheckBadShift ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Array Out of Bounds: "
              << (CheckArrayOOB ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Dead Branch: "
              << (CheckDeadBranch ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Robust Reachability: "
              << (m_robust_reachability ? "Enabled" : "Disabled");
  if (!m_dump_ef_path.empty()) {
    MKINT_LOG() << "  Dump EF Constraints: " << m_dump_ef_path;
  }
  if (m_robust_reachability) {
    MKINT_LOG() << "  Robust Universals: unknown-loads="
                << (m_robust_universal_unknown_loads ? "on" : "off")
                << ", external-globals="
                << (m_robust_universal_external_globals ? "on" : "off")
                << ", inline-asm="
                << (m_robust_universal_inline_asm ? "on" : "off");
    if (!m_robust_bug_filter.empty()) {
      MKINT_LOG() << "  Robust Bug Filter: custom list";
    }
  }

  // Warn if no checkers are enabled
  if (!CheckIntOverflow && !CheckDivByZero && !CheckBadShift &&
      !CheckArrayOOB && !CheckDeadBranch) {
    MKINT_WARN() << "No bug checkers are enabled. No bugs will be detected.";
    MKINT_WARN() << "Select one or more checkers with --checks=<id[,id...]>";
  }

  // FIXME: This is a hack.
  auto *ctx = new z3::context; // let it leak.
  m_solver = z3::solver(*ctx);
  m_dl = &M.getDataLayout();
  m_ptr_bits = m_dl->getPointerSizeInBits(0);
  m_smt_mem = std::make_unique<SmtMemory>(*ctx, m_ptr_bits);
  m_module = &M;
  m_func2tsrc.clear();
  m_taint_funcs.clear();
  m_backedges.clear();
  m_callback_tsrc_fn.clear();
  m_impossible_branches.clear();
  m_gep_oob.clear();
  m_overflow_insts.clear();
  m_bad_shift_insts.clear();
  m_div_zero_insts.clear();
  if (m_bug_detection) {
    m_bug_detection->clearState();
  }
  m_obj_base.clear();
  m_obj_size.clear();
  m_obj_list.clear();
  m_obj_mem.clear();
  m_obj_alias.clear();
  m_int_alias.clear();
  m_ptr_offset.clear();
  m_int_offset.clear();
  m_obj_freed.clear();
  m_object_frames.clear();
  m_bbpaths.clear();
  m_v2sym.clear();
  m_aa = nullptr;
  m_mssa = nullptr;
  m_fam = nullptr;
  m_sym_change_log.clear();
  m_sym_change_frames.clear();
  m_path_constraints.clear();
  m_constraint_frames.clear();
  m_universal_vars.clear();
  m_universal_var_ids.clear();
  m_summary_cache.clear();
  m_building_summary = nullptr;
  m_summary_failed = false;
  m_summary_failure_reason.clear();

  // Mark taint sources.
  for (auto &F : M) {
    auto taint_sources = m_taint_analysis->get_taint_source(F);
    m_taint_analysis->mark_func_sinks(F, m_callback_tsrc_fn);
    if (TaintAnalysis::is_taint_src(F.getName()))
      m_func2tsrc[&F] = std::move(taint_sources);
  }

  // Propagate taint across functions
  m_taint_analysis->propagate_taint_across_functions(M, m_func2tsrc,
                                                     m_taint_funcs);

  // Also add main function to analysis if it exists and is not already in
  // taint_funcs
  for (auto &F : M) {
    if (!F.isDeclaration()) {
      backedge_analysis(F);
      // Add main function to analysis if it's not already there
      if (F.getName() == "main" && !m_taint_funcs.contains(&F)) {
        m_taint_funcs.insert(&F);
        MKINT_LOG() << "Added main function to analysis";
      }
    }
  }

  MKINT_LOG() << "Module after taint:";
  MKINT_LOG() << M;

  const DataLayout &DL = M.getDataLayout();
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  m_fam = &FAM;

  this->smt_solving(M);

  m_bug_detection->mark_errors(m_impossible_branches, m_gep_oob,
                               m_overflow_insts, m_bad_shift_insts,
                               m_div_zero_insts);

  // Report bugs to BugReportMgr (shared pattern)
  reportBugsToManager();

  // Note: SARIF/JSON output is now handled centrally by BugReportMgr
  // in the tool driver, not by individual checkers

  return PreservedAnalyses::all();
}

void MKintPass::buildSummaries(Module &M) {
  if (InterprocSummaryMode == SummaryMode::Off)
    return;
  for (auto &F : M) {
    if (!F.isDeclaration())
      (void)buildSummary(F);
  }
}

void MKintPass::smt_solving(Module &M) {
  SetVector<Function *> funcs_to_analyze;
  if (AnalyzeAllFunctions) {
    for (auto &F : M) {
      if (!F.isDeclaration())
        funcs_to_analyze.insert(&F);
    }
  }
  for (auto *F : m_taint_funcs)
    funcs_to_analyze.insert(F);

  buildSummaries(M);

  for (auto *F : funcs_to_analyze) {
    if (F->isDeclaration())
      continue;

    // Reset per-function SMT state (kept inside the solver push/pop).
    m_bbpaths.clear();
    m_v2sym.clear();
    m_smt_mem->reset();
    m_obj_base.clear();
    m_obj_size.clear();
    m_obj_list.clear();
    m_obj_mem.clear();
    m_obj_alias.clear();
    m_int_alias.clear();
    m_ptr_offset.clear();
    m_int_offset.clear();
    m_obj_freed.clear();
    m_object_frames.clear();
    m_sym_change_log.clear();
    m_sym_change_frames.clear();
    m_path_constraints.clear();
    m_constraint_frames.clear();
    m_universal_vars.clear();
    m_universal_var_ids.clear();
    if (m_bug_detection) {
      m_bug_detection->clearCurrentPath();
    }

    // Record start time for this function
    m_function_start_time = std::chrono::steady_clock::now();
    m_paths_explored = 0;
    m_path_limit_hit = false;
    m_timeout_hit = false;
    m_summary_backedge_hit = false;
    MKINT_LOG() << "Beginning analysis of function " << F->getName();

    if (m_fam && (m_aa = m_fam->getCachedResult<llvm::AAManager>(*F))) {
      // cached
    } else if (m_fam) {
      m_aa = &m_fam->getResult<llvm::AAManager>(*F);
    } else {
      m_aa = nullptr;
    }
    if (m_fam) {
      if (auto *MSSARes = m_fam->getCachedResult<llvm::MemorySSAAnalysis>(*F)) {
        m_mssa = &MSSARes->getMSSA();
      } else {
        m_mssa = &m_fam->getResult<llvm::MemorySSAAnalysis>(*F).getMSSA();
      }
    } else {
      m_mssa = nullptr;
    }

    // Seed global objects (base address + size) so pointer arithmetic and
    // loads/stores have a model.
    for (auto &GV : M.globals()) {
      const uint64_t bytes = m_dl->getTypeAllocSize(GV.getValueType());
      ensureObject(&GV, ("global." + GV.getName()).str(),
                   m_solver.value().ctx().bv_val(bytes, m_ptr_bits),
                   /*sizeKnown=*/true);
    }

    // Seed stack objects (allocas) with constant sizes when possible.
    for (auto &bb : F->getBasicBlockList()) {
      for (auto &inst : bb) {
        if (auto *ai = dyn_cast<AllocaInst>(&inst)) {
          const uint64_t elemBytes =
              m_dl->getTypeAllocSize(ai->getAllocatedType());
          z3::expr sizeBytesExpr =
              m_solver.value().ctx().bv_val(elemBytes, m_ptr_bits);
          bool known = true;
          if (ai->isArrayAllocation()) {
            if (auto *ci = dyn_cast<ConstantInt>(ai->getArraySize())) {
              sizeBytesExpr = m_solver.value().ctx().bv_val(
                  elemBytes * ci->getZExtValue(), m_ptr_bits);
            } else {
              auto countExpr =
                  getIntExpr(ai->getArraySize(), &F->getEntryBlock(), nullptr);
              const unsigned cbw = countExpr.get_sort().bv_size();
              if (cbw < m_ptr_bits)
                countExpr = z3::zext(countExpr, m_ptr_bits - cbw);
              else if (cbw > m_ptr_bits)
                countExpr = countExpr.extract(m_ptr_bits - 1, 0);
              sizeBytesExpr = countExpr * m_solver.value().ctx().bv_val(
                                              elemBytes, m_ptr_bits);
              known = countExpr.is_numeral();
            }
          }
          ensureObject(ai,
                       ("alloca." + F->getName().str() + "." +
                        std::to_string((uintptr_t)ai)),
                       sizeBytesExpr, known);
        }
      }
    }

    // Get a path tree.
    for (auto &bb : F->getBasicBlockList()) {
      for (const auto &pred : predecessors(&bb)) {
        if (m_backedges[&bb].contains(pred) || &bb == pred)
          continue;

        m_bbpaths[pred].push_back(&bb);
      }
    }

    m_solver.value().push();
    pushSymFrame();
    pushObjectFrame();
    m_smt_mem->push();
    pushConstraintFrame();

    // add function arg constraints (integers + pointers).
    for (auto &arg : F->args()) {
      const std::string arg_name =
          (F->getName() + "." + std::to_string(arg.getArgNo())).str();
      if (arg.getType()->isIntegerTy()) {
        const auto argv = m_solver.value().ctx().bv_const(
            arg_name.c_str(), arg.getType()->getIntegerBitWidth());
        setSym(&arg, argv);
      } else if (arg.getType()->isPointerTy()) {
        const auto sizev = m_solver.value().ctx().bv_const(
            (arg_name + ".size").c_str(), m_ptr_bits);
        ensureObject(&arg, arg_name + ".obj", sizev, /*sizeKnown=*/false);
        setSym(&arg, m_obj_base[&arg].value());
      }
    }

    path_solving(&(F->getEntryBlock()), nullptr);

    m_smt_mem->pop();
    popSymFrame();
    popObjectFrame();
    popConstraintFrame();
    m_solver.value().pop();

    // Report analysis time
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - m_function_start_time)
                       .count();
    MKINT_LOG() << "Completed analysis of function " << F->getName() << " in "
                << elapsed << " seconds";
  }
}

void MKintPass::generateSarifReport(const std::string &filename) {
  if (m_bug_detection) {
    m_bug_detection->generateSarifReport(filename, m_impossible_branches,
                                         m_gep_oob, m_overflow_insts,
                                         m_bad_shift_insts, m_div_zero_insts);
  }
}

} // namespace kint
