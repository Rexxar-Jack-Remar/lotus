
#include "Checker/Pulse/PulseChecker.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Checker/Pulse/PulseDiagnostic.h"
#include "Checker/Pulse/PulseFormula.h"
#include "Checker/Pulse/PulseInvalidation.h"
#include "Checker/Pulse/PulseLogger.h"
#include "Checker/Pulse/PulseModels.h"
#include "Checker/Pulse/PulseReport.h"
#include "Checker/Pulse/PulseSpecialization.h"
#include "Checker/Pulse/PulseSubstitution.h"
#include "Checker/Pulse/PulseTaint.h"
#include "Checker/Report/BugReportMgr.h"

#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

//===----------------------------------------------------------------------===//
// PulseChecker (Infer Pulse-inspired "incorrectness logic")
//
// This driver performs a bounded disjunctive symbolic execution over LLVM IR.
// Key properties:
// - The analysis aims at *witnessable* bugs: reports should correspond to at
//   least one feasible execution (sound incorrectness), rather than "may" bugs.
// - The abstract state is biabductive (see `AbductiveDomain`): reads may
//   populate preconditions that later become requirements on callers.
// - At control-flow merges, path conditions must be joined as a disjunction
//   approximation (keep stable facts), not conjoined.
// - Path explosion is controlled via `kMaxDisjuncts`, widening, and summary
//   construction; these knobs trade recall for scalability.
//===----------------------------------------------------------------------===//

namespace {
static bool isNullPointerConstantValue(const llvm::Value *v) {
  if (!v)
    return false;
  if (llvm::isa<llvm::ConstantPointerNull>(v))
    return true;
  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
    if (CE->getOpcode() == llvm::Instruction::IntToPtr) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0)))
        return CI->isZero();
    }
  }
  if (auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(v)) {
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(I2P->getOperand(0)))
      return CI->isZero();
  }
  return false;
}

static llvm::Optional<int64_t> getI64Constant(const llvm::Value *v,
                                              bool is_signed) {
  if (!v)
    return llvm::None;
  auto *CI = llvm::dyn_cast<llvm::ConstantInt>(v);
  if (!CI)
    return llvm::None;
  if (CI->getBitWidth() > 64)
    return llvm::None;
  if (is_signed)
    return CI->getSExtValue();
  // For unsigned, reject values that don't fit in signed int64_t (we keep
  // int64_t bounds in the formula).
  if (CI->getBitWidth() == 64 && CI->isNegative())
    return llvm::None;
  return static_cast<int64_t>(CI->getZExtValue());
}

static llvm::ICmpInst::Predicate invertIcmpPred(llvm::ICmpInst::Predicate p) {
  using P = llvm::ICmpInst::Predicate;
  switch (p) {
  case P::ICMP_EQ:
    return P::ICMP_NE;
  case P::ICMP_NE:
    return P::ICMP_EQ;
  case P::ICMP_SLT:
    return P::ICMP_SGE;
  case P::ICMP_SLE:
    return P::ICMP_SGT;
  case P::ICMP_SGT:
    return P::ICMP_SLE;
  case P::ICMP_SGE:
    return P::ICMP_SLT;
  case P::ICMP_ULT:
    return P::ICMP_UGE;
  case P::ICMP_ULE:
    return P::ICMP_UGT;
  case P::ICMP_UGT:
    return P::ICMP_ULE;
  case P::ICMP_UGE:
    return P::ICMP_ULT;
  default:
    return p;
  }
}

static llvm::ICmpInst::Predicate swapIcmpSides(llvm::ICmpInst::Predicate p) {
  using P = llvm::ICmpInst::Predicate;
  switch (p) {
  case P::ICMP_SLT:
    return P::ICMP_SGT;
  case P::ICMP_SLE:
    return P::ICMP_SGE;
  case P::ICMP_SGT:
    return P::ICMP_SLT;
  case P::ICMP_SGE:
    return P::ICMP_SLE;
  case P::ICMP_ULT:
    return P::ICMP_UGT;
  case P::ICMP_ULE:
    return P::ICMP_UGE;
  case P::ICMP_UGT:
    return P::ICMP_ULT;
  case P::ICMP_UGE:
    return P::ICMP_ULE;
  default:
    return p;
  }
}

static bool applyIntegerIcmpConstraint(PulseFormula &formula,
                                       llvm::ICmpInst::Predicate p,
                                       AbstractValue lhs, AbstractValue rhs) {
  using P = llvm::ICmpInst::Predicate;
  formula.addIntegerConstraint(lhs);
  formula.addIntegerConstraint(rhs);

  // If comparing against a constant, tighten bounds.
  const llvm::Value *rhs_v = rhs.getValue();
  const llvm::Value *lhs_v = lhs.getValue();

  // Normalize to keep the constant on the RHS when possible.
  bool rhs_is_const = llvm::isa<llvm::ConstantInt>(rhs_v);
  bool lhs_is_const = llvm::isa<llvm::ConstantInt>(lhs_v);
  if (lhs_is_const && !rhs_is_const) {
    std::swap(lhs, rhs);
    std::swap(lhs_v, rhs_v);
    p = swapIcmpSides(p);
    rhs_is_const = llvm::isa<llvm::ConstantInt>(rhs_v);
  }

  if (p == P::ICMP_EQ)
    return formula.addEquality(lhs, rhs);
  if (p == P::ICMP_NE)
    return formula.addDisequality(lhs, rhs);

  const bool is_signed = (p == P::ICMP_SLT || p == P::ICMP_SLE ||
                          p == P::ICMP_SGT || p == P::ICMP_SGE);

  if (rhs_is_const) {
    auto c_opt = getI64Constant(rhs_v, is_signed);
    if (!c_opt)
      return true; // Can't encode precisely.
    const int64_t c = *c_opt;

    auto add_lower = [&](int64_t lb) { return formula.addBounds(lhs, lb, std::numeric_limits<int64_t>::max()); };
    auto add_upper = [&](int64_t ub) { return formula.addBounds(lhs, std::numeric_limits<int64_t>::min(), ub); };

    switch (p) {
    case P::ICMP_SLT:
    case P::ICMP_ULT:
      if (c == std::numeric_limits<int64_t>::min())
        return false;
      return add_upper(c - 1);
    case P::ICMP_SLE:
    case P::ICMP_ULE:
      return add_upper(c);
    case P::ICMP_SGT:
    case P::ICMP_UGT:
      if (c == std::numeric_limits<int64_t>::max())
        return false;
      return add_lower(c + 1);
    case P::ICMP_SGE:
    case P::ICMP_UGE:
      return add_lower(c);
    default:
      return true;
    }
  }

  // Variable-vs-variable inequalities: encode as linear constraint lhs - rhs ? 0.
  LinearConstraint c;
  c.terms.emplace_back(lhs, 1);
  c.terms.emplace_back(rhs, -1);
  c.constant = 0;
  switch (p) {
  case P::ICMP_SLT:
  case P::ICMP_ULT:
    c.kind = ConstraintKind::Less;
    break;
  case P::ICMP_SLE:
  case P::ICMP_ULE:
    c.kind = ConstraintKind::LessEqual;
    break;
  case P::ICMP_SGT:
  case P::ICMP_UGT:
    c.kind = ConstraintKind::Greater;
    break;
  case P::ICMP_SGE:
  case P::ICMP_UGE:
    c.kind = ConstraintKind::GreaterEqual;
    break;
  default:
    return true;
  }
  return formula.addLinearConstraint(c);
}
} // namespace

constexpr unsigned PulseChecker::kMaxDisjuncts;
constexpr unsigned PulseChecker::kMaxCallDepth;

PulseChecker::PulseChecker(llvm::Module *M, lotus::AliasAnalysisWrapper *AA)
    : module_(M), aa_(AA), ops_(&factory_),
      models_(std::make_unique<PulseModels>(*this)) {
  registerBugTypes();
  if (aa_ && aa_->isInitialized()) {
    factory_.setMustAliasFn(
        [this](const llvm::Value *v1, const llvm::Value *v2) {
          return aa_->mustAlias(v1, v2);
        });
  }
}

PulseChecker::~PulseChecker() {
  // Report any remaining latent issues before flushing
  for (const auto &latent : latent_issues_) {
    Trace trace = latent.getTrace();
    OperationResult kind = latent.getDiagnostic();
    const llvm::Instruction *loc = latent.getLocation();
    AbstractValue addr = latent.getAddress();

    // Create a dummy astate for reporting (latent issues don't have astate)
    // We'll report without astate context
    reportBug(kind, loc, addr, trace, nullptr);
  }

  // Flush diagnostics at end of analysis
  DiagnosticManager::getInstance().flush();
}

void PulseChecker::registerBugTypes() {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  auto &diagMgr = DiagnosticManager::getInstance();

  useAfterFreeTypeId_ =
      mgr.register_bug_type(IssueType::UseAfterFree, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-416");
  diagMgr.registerBugType(IssueType::UseAfterFree, useAfterFreeTypeId_);

  nullDerefTypeId_ =
      mgr.register_bug_type(IssueType::NullDereference, BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR, "CWE-476");
  diagMgr.registerBugType(IssueType::NullDereference, nullDerefTypeId_);

  uninitializedReadTypeId_ = mgr.register_bug_type(
      IssueType::UninitializedRead, BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "CWE-457");
  diagMgr.registerBugType(IssueType::UninitializedRead,
                          uninitializedReadTypeId_);

  unnecessaryCopyTypeId_ =
      mgr.register_bug_type(IssueType::UnnecessaryCopy, BugDescription::BI_LOW,
                            BugDescription::BC_PERFORMANCE, "");
  diagMgr.registerBugType(IssueType::UnnecessaryCopy, unnecessaryCopyTypeId_);

  constRefableParamTypeId_ =
      mgr.register_bug_type("Const-Refable Parameter", BugDescription::BI_LOW,
                            BugDescription::BC_PERFORMANCE, "");
  // Note: Const-refable param doesn't have a constant in PulseDiagnostic yet,
  // or we can add one. For now, skipping registration in diagMgr or adding it.

  taintErrorTypeId_ =
      mgr.register_bug_type(IssueType::TaintError, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-20");
  diagMgr.registerBugType(IssueType::TaintError, taintErrorTypeId_);

  stackAddressEscapeTypeId_ = mgr.register_bug_type(
      IssueType::StackVariableAddressEscape, BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY, "CWE-562");
  diagMgr.registerBugType(IssueType::StackVariableAddressEscape,
                          stackAddressEscapeTypeId_);

  invalidFreeTypeId_ =
      mgr.register_bug_type(IssueType::InvalidFree, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-590");
  diagMgr.registerBugType(IssueType::InvalidFree, invalidFreeTypeId_);
}

void PulseChecker::analyze() {
  PulseLogger::info("Starting module analysis");
  PulseLogger::incrementCounter("modules.analyzed");

  std::vector<const llvm::Function *> functions;
  functions.reserve(module_->size());
  for (auto &F : *module_) {
    if (!F.isDeclaration())
      functions.push_back(&F);
  }

  std::unordered_set<const llvm::Function *> function_set(functions.begin(),
                                                          functions.end());

  // Build direct call graph: caller -> callee (only for defined functions).
  std::unordered_map<const llvm::Function *, std::vector<const llvm::Function *>>
      edges;
  edges.reserve(functions.size());
  for (const llvm::Function *F : functions) {
    std::vector<const llvm::Function *> callees;
    for (const auto &BB : *F) {
      for (const auto &I : BB) {
        auto *CI = llvm::dyn_cast<llvm::CallInst>(&I);
        if (!CI)
          continue;
        const llvm::Function *Callee = CI->getCalledFunction();
        if (!Callee || Callee->isDeclaration())
          continue;
        if (function_set.count(Callee) == 0)
          continue;
        callees.push_back(Callee);
      }
    }
    edges.emplace(F, std::move(callees));
  }

  // Reverse edges for SCC computation (Kosaraju).
  std::unordered_map<const llvm::Function *, std::vector<const llvm::Function *>>
      rev_edges;
  rev_edges.reserve(functions.size());
  for (const llvm::Function *F : functions) {
    (void)rev_edges[F];
  }
  for (const auto &kv : edges) {
    const llvm::Function *caller = kv.first;
    for (const llvm::Function *callee : kv.second) {
      rev_edges[callee].push_back(caller);
    }
  }

  // First DFS pass: finish order.
  std::vector<const llvm::Function *> order;
  order.reserve(functions.size());
  std::unordered_set<const llvm::Function *> visited;
  visited.reserve(functions.size());

  std::function<void(const llvm::Function *)> dfs1 =
      [&](const llvm::Function *F) {
        if (visited.count(F))
          return;
        visited.insert(F);
        auto it = edges.find(F);
        if (it != edges.end()) {
          for (const llvm::Function *callee : it->second) {
            dfs1(callee);
          }
        }
        order.push_back(F);
      };

  for (const llvm::Function *F : functions)
    dfs1(F);

  // Second DFS pass on reverse graph: collect SCCs.
  std::unordered_set<const llvm::Function *> visited2;
  visited2.reserve(functions.size());
  std::vector<std::vector<const llvm::Function *>> sccs;
  sccs.reserve(functions.size());

  std::function<void(const llvm::Function *, std::vector<const llvm::Function *> &)>
      dfs2 = [&](const llvm::Function *F,
                 std::vector<const llvm::Function *> &scc) {
        if (visited2.count(F))
          return;
        visited2.insert(F);
        scc.push_back(F);
        auto it = rev_edges.find(F);
        if (it != rev_edges.end()) {
          for (const llvm::Function *pred : it->second) {
            dfs2(pred, scc);
          }
        }
      };

  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const llvm::Function *F = *it;
    if (visited2.count(F))
      continue;
    std::vector<const llvm::Function *> scc;
    dfs2(F, scc);
    sccs.push_back(std::move(scc));
  }

  // Map function -> SCC id.
  std::unordered_map<const llvm::Function *, size_t> scc_id;
  scc_id.reserve(functions.size());
  for (size_t i = 0; i < sccs.size(); ++i) {
    for (const llvm::Function *F : sccs[i])
      scc_id[F] = i;
  }

  // Build SCC DAG (caller SCC -> callee SCC), then topo-sort it.
  std::vector<std::unordered_set<size_t>> scc_edges(sccs.size());
  std::vector<size_t> indeg(sccs.size(), 0);
  for (const auto &kv : edges) {
    const llvm::Function *caller = kv.first;
    size_t c_id = scc_id[caller];
    for (const llvm::Function *callee : kv.second) {
      size_t d_id = scc_id[callee];
      if (c_id == d_id)
        continue;
      if (scc_edges[c_id].insert(d_id).second) {
        indeg[d_id]++;
      }
    }
  }

  std::queue<size_t> q;
  for (size_t i = 0; i < indeg.size(); ++i) {
    if (indeg[i] == 0)
      q.push(i);
  }
  std::vector<size_t> topo;
  topo.reserve(sccs.size());
  while (!q.empty()) {
    size_t id = q.front();
    q.pop();
    topo.push_back(id);
    for (size_t succ : scc_edges[id]) {
      if (--indeg[succ] == 0)
        q.push(succ);
    }
  }

  // Analyze SCCs in reverse-topological order (callees first).
  for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
    size_t id = *it;
    current_scc_.clear();
    for (const llvm::Function *F : sccs[id])
      current_scc_.insert(F);
    for (const llvm::Function *F : sccs[id]) {
      analyzeFunction(F);
    }
  }
  current_scc_.clear();

  PulseLogger::info("Completed analysis of " +
                    std::to_string(functions.size()) +
                    " functions");

  // Flush periodically or at end
  DiagnosticManager::getInstance().flush();
}

void PulseChecker::analyzeFunction(const llvm::Function *F) {
  PulseLogger::logFunction(F, "starting analysis");
  PulseLogger::startTimer("function." + F->getName().str());
  PulseLogger::incrementCounter("functions.analyzed");

  // Skip if already has summary (avoid re-analysis)
  if (summary_manager_.hasSummary(F)) {
    PulseLogger::logFunction(F, "skipped (already has summary)");
    return;
  }

  analysis_non_disj_.clear();

  LoopAbstraction loop_abs;
  {
    llvm::Function &Fn = *const_cast<llvm::Function *>(F);
    llvm::DominatorTree DT(Fn);
    llvm::LoopInfo LI;
    LI.analyze(DT);
    loop_abs.initialize(LI);
  }
  loop_abstractions_[F] = std::move(loop_abs);
  LoopAbstraction &loop_abs_ref = loop_abstractions_[F];

  // Initialize disjunctive domain
  DisjunctiveDomain &disj_domain = disjunctive_domains_[F];
  disj_domain.clear();

  using WorkItem = std::tuple<const llvm::BasicBlock *, ExecutionDomain,
                              const llvm::BasicBlock *>;
  std::queue<WorkItem> worklist;
  const llvm::BasicBlock *entry_block = &F->getEntryBlock();
  ExecutionDomain init_state = initializeFunction(F);
  worklist.push(std::make_tuple(entry_block, std::move(init_state),
                                (const llvm::BasicBlock *)nullptr));

  std::vector<ExecutionDomain> exit_states;
  std::vector<ExecutionDomain> latent_exit_states;
  std::map<const llvm::BasicBlock *, unsigned> block_visits;
  unsigned iter_limit = 0;
  const unsigned max_iter = 100000;

  PulseLogger::incrementCounter("paths.explored");

  while (!worklist.empty() && iter_limit++ < max_iter) {
    if (worklist.size() > kMaxDisjuncts) {
      PulseLogger::warning("Reached max disjuncts limit for function " +
                           F->getName().str());
      break;
    }

    WorkItem item = std::move(worklist.front());
    worklist.pop();
    const llvm::BasicBlock *BB = std::get<0>(item);
    ExecutionDomain current_state = std::move(std::get<1>(item));
    const llvm::BasicBlock *pred_bb = std::get<2>(item);

    if (current_state.isStopped())
      continue;

    // Record entry predecessor for sound PHI/select handling.
    current_state.setEntryPred(pred_bb);

    block_visits[BB]++;
    if (block_visits[BB] > kMaxDisjuncts * 4)
      continue;

    if (loop_abs_ref.isLoopHeader(BB)) {
      bool should_widen = loop_abs_ref.visitHeader(BB, current_state);

      if (should_widen) {
        // Check if we should infer invariant
        if (loop_abs_ref.isInferringInvariant(BB)) {
          auto invariant_opt = loop_abs_ref.inferInvariant(
              BB, loop_abs_ref.getEntryState(BB), current_state);
          if (invariant_opt) {
            // Only adopt an inferred invariant if it preserves the current
            // incoming edge context. Otherwise, PHI nodes could be evaluated
            // with a predecessor different from the witness that reached BB.
            if (invariant_opt->getEntryPred() == pred_bb) {
              current_state = std::move(*invariant_opt);
            }
          }
        } else {
          // Apply widening
          ExecutionDomain widened = loop_abs_ref.widen(BB, current_state);
          if (widened.getEntryPred() == pred_bb) {
            current_state = std::move(widened);
          }
        }
        if (current_state.isStopped()) {
          continue;
        }
      }
    }

    // Track iteration for widening
    disj_domain.widen(BB);

    // Check if we should widen at this block
    if (disj_domain.shouldWiden(BB)) {
      // After widening, join all disjuncts at this block
      ExecutionDomain joined = disj_domain.joinAtBlock(BB);
      if (!joined.isStopped()) {
        current_state = std::move(joined);
        pred_bb = current_state.getEntryPred();
      } else {
        continue;
      }
    } else {
      // Add current state to disjunctive domain for this block
      disj_domain.add(BB, current_state.clone(), pred_bb);
    }

    for (const llvm::Instruction &I : *BB) {
      if (current_state.isStopped())
        break;
      if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
        (void)RI;
        if (current_state.isExitProgram() ||
            current_state.isContinueProgram()) {
          exit_states.push_back(current_state.clone());
        }
        break;
      }

      const llvm::BasicBlock *phi_pred = pred_bb;
      if (llvm::isa<llvm::PHINode>(&I) && !phi_pred) {
        auto it = pred_begin(BB);
        if (it != pred_end(BB))
          phi_pred = *it;
      }

      auto new_states = executeInstruction(&I, current_state, phi_pred, 0u);
      if (new_states.empty() || new_states[0].isStopped()) {
        current_state = new_states.empty() ? std::move(current_state)
                                           : std::move(new_states[0]);
        break;
      }
      current_state = std::move(new_states[0]);
    }

    if (current_state.isStopped()) {
      // Handle stopped states: if it's ExitProgram, add to exit_states
      if (current_state.isExitProgram()) {
        exit_states.push_back(current_state.clone());
      } else if (current_state.isLatentAbortProgram() ||
                 current_state.isLatentInvalidAccess()) {
        latent_exit_states.push_back(current_state.clone());
      }
      // For AbortProgram, we've already reported
      continue;
    }

    const llvm::Instruction *term = BB->getTerminator();
    if (!term)
      continue;

    auto *BI =
        llvm::dyn_cast<llvm::BranchInst>(const_cast<llvm::Instruction *>(term));
    if (BI && BI->isConditional() && BI->getNumSuccessors() == 2) {
      for (unsigned i = 0; i < 2; i++) {
        llvm::Optional<ExecutionDomain> fork_opt =
            applyBranchCondition(current_state.clone(), BI, i, pred_bb);
        if (!fork_opt)
          continue;
        const llvm::BasicBlock *succ = BI->getSuccessor(i);
        if (succ->empty())
          continue;
        fork_opt->setEntryPred(BB);
        worklist.push(std::make_tuple(succ, std::move(*fork_opt), BB));
      }
    } else {
      for (const llvm::BasicBlock *succ : llvm::successors(BB)) {
        if (succ->empty())
          continue;
        ExecutionDomain succ_state = current_state.clone();
        succ_state.setEntryPred(BB);
        worklist.push(std::make_tuple(succ, std::move(succ_state), BB));
      }
    }
  }

  if (!exit_states.empty() || !latent_exit_states.empty())
    createSummary(F, exit_states, latent_exit_states);

  reportUnnecessaryCopies(F);
  reportConstRefableParams(F);

  PulseLogger::endTimer("function." + F->getName().str());
  PulseLogger::logFunction(F, "completed analysis");
}

ExecutionDomain PulseChecker::initializeFunction(const llvm::Function *F) {
  ExecutionDomain exec_state;
  auto *astate = exec_state.getAstate();
  for (auto &Arg : F->args()) {
    if (Arg.getType()->isPointerTy()) {
      AbstractValue av = factory_.getOrCreate(&Arg);
      Address addr(av);
      astate->getPostStack().add(&Arg, addr);
      astate->getPostAttrs().add(av, Attribute::Uninitialized);
    }
  }
  return exec_state;
}

std::vector<ExecutionDomain> PulseChecker::executeInstruction(
    const llvm::Instruction *I, ExecutionDomain exec_state,
    const llvm::BasicBlock *pred, unsigned call_depth) {
  if (exec_state.isStopped())
    return {exec_state};
  auto *astate = exec_state.getAstate();
  if (!astate)
    return {exec_state};

  auto pruneInfeasible = [](ExecutionDomain &st) -> bool {
    if (st.isStopped())
      return true;
    AbductiveDomain *a = st.getAstate();
    if (!a)
      return true;
    const PulseFormula &f = a->getPathFormula();
    return f.isConsistent() && !f.isUnsat();
  };

  auto pruneStates = [&](std::vector<ExecutionDomain> states)
      -> std::vector<ExecutionDomain> {
    std::vector<ExecutionDomain> out;
    out.reserve(states.size());
    for (auto &st : states) {
      if (pruneInfeasible(st)) {
        out.push_back(std::move(st));
      }
    }
    return out;
  };

  //===--------------------------------------------------------------------===//
  // Select
  //
  // `select cond, t, f` is a value-level merge. For sound incorrectness, we do
  // not want to conflate the two alternatives. However, this engine explores a
  // single witness state per instruction step, so we pick a *single* feasible
  // branch:
  // - If the condition is provably true/false (using our limited formula),
  //   choose the corresponding operand.
  // - Otherwise, pick a deterministic representative (true operand).
  //
  // This may miss bugs (false negatives) but will not fabricate a witness.
  if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(I)) {
    if (Sel->getType()->isPointerTy()) {
      llvm::Optional<bool> truth = llvm::None;
      if (auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(Sel->getCondition())) {
        llvm::Value *op0 = ICmp->getOperand(0);
        llvm::Value *op1 = ICmp->getOperand(1);
        const bool op0_is_null = isNullPointerConstantValue(op0);
        const bool op1_is_null = isNullPointerConstantValue(op1);
        llvm::ICmpInst::Predicate icmp_pred = ICmp->getPredicate();

        // Decide select condition only when we can prove it.
        if (op0_is_null || op1_is_null) {
          llvm::Value *ptr = op0_is_null ? op1 : op0;
          auto ptr_opt = ops_.eval(*astate, ptr, I, pred);
          if (ptr_opt) {
            AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
            const bool proven_null = astate->getPathFormula().isNull(canon_ptr);
            const bool proven_non_null =
                astate->getPathFormula().isNonNull(canon_ptr);
            if (icmp_pred == llvm::ICmpInst::ICMP_EQ) {
              if (proven_null)
                truth = true;
              else if (proven_non_null)
                truth = false;
            } else if (icmp_pred == llvm::ICmpInst::ICMP_NE) {
              if (proven_non_null)
                truth = true;
              else if (proven_null)
                truth = false;
            }
          }
        } else if (op0->getType()->isPointerTy() && op1->getType()->isPointerTy()) {
          auto v0_opt = ops_.eval(*astate, op0, I, pred);
          auto v1_opt = ops_.eval(*astate, op1, I, pred);
          if (v0_opt && v1_opt) {
            AbstractValue c0 = astate->getCanonical(v0_opt->addr);
            AbstractValue c1 = astate->getCanonical(v1_opt->addr);
            const bool proven_eq = astate->getPathFormula().areEqual(c0, c1);
            const bool proven_neq =
                astate->getPathFormula().areDisequal(c0, c1);
            if (icmp_pred == llvm::ICmpInst::ICMP_EQ) {
              if (proven_eq)
                truth = true;
              else if (proven_neq)
                truth = false;
            } else if (icmp_pred == llvm::ICmpInst::ICMP_NE) {
              if (proven_neq)
                truth = true;
              else if (proven_eq)
                truth = false;
            }
          }
        }
      }

      const llvm::Value *chosen =
          (truth && *truth) ? Sel->getTrueValue() : Sel->getFalseValue();
      if (!truth) {
        // Unknown condition: pick deterministic witness.
        chosen = Sel->getTrueValue();
      }

      auto chosen_opt = ops_.eval(*astate, chosen, I, pred);
      if (chosen_opt) {
        AbstractValue canon = astate->getCanonical(chosen_opt->addr);
        Address result(canon);
        result.history = chosen_opt->history;
        result.history.addEvent(ValueHistory::EventKind::Unknown, I,
                                I->getFunction());
        astate->getPostStack().add(Sel, result);
      }
    }
    return pruneStates({exec_state});
  }

  if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(I)) {
    // PHI nodes need special handling: they merge values from multiple
    // predecessors Since we've already joined states at block entry, we can use
    // any predecessor In a more precise implementation, we'd track which
    // predecessor led to which state
    if (!pred) {
      pred = exec_state.getEntryPred();
    }

    // Check if pred is actually a predecessor of the PHI node
    bool is_predecessor = false;
    for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
      if (Phi->getIncomingBlock(i) == pred) {
        is_predecessor = true;
        break;
      }
    }
    if (!is_predecessor) {
      // Without a matching predecessor, do not guess an incoming edge: it can
      // fabricate a witness. Fall back to an unknown fresh value.
      AbstractValue fresh = factory_.createFresh(Phi);
      Address addr(fresh);
      addr.history.addEvent(ValueHistory::EventKind::Unknown, I,
                            I->getFunction());
      astate->getPostStack().add(Phi, addr);
      return pruneStates({exec_state});
    }

    const llvm::Value *incoming = Phi->getIncomingValueForBlock(pred);
    auto addr_opt = ops_.eval(*astate, incoming, I, pred);
    if (addr_opt) {
      // Canonicalize the PHI result
      AbstractValue canon_addr = astate->getCanonical(addr_opt->addr);
      Address canon_result(canon_addr);
      canon_result.history = addr_opt->history;
      canon_result.history.addEvent(ValueHistory::EventKind::Unknown, I,
                                    I->getFunction());
      astate->getPostStack().add(Phi, canon_result);
    }
    return pruneStates({exec_state});
  }

  if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(I))
    return pruneStates({handleLoad(LI, exec_state, pred)});
  if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(I))
    return pruneStates({handleStore(SI, exec_state, pred)});
  if (auto *CI = llvm::dyn_cast<llvm::CallInst>(I))
    return pruneStates(handleCall(CI, exec_state, pred, call_depth));
  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(I))
    return pruneStates({handleAlloca(AI, exec_state)});
  if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(I))
    return pruneStates({handleReturn(RI, exec_state)});

  // Integer arithmetic: record constraints linking SSA values.
  if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(I)) {
    if (BO->getType()->isIntegerTy()) {
      auto lhs_opt = ops_.eval(*astate, BO->getOperand(0), BO, pred);
      auto rhs_opt = ops_.eval(*astate, BO->getOperand(1), BO, pred);
      if (lhs_opt && rhs_opt) {
        AbstractValue res_av = factory_.getOrCreate(BO);
        AbstractValue lhs_av = astate->getCanonical(lhs_opt->addr);
        AbstractValue rhs_av = astate->getCanonical(rhs_opt->addr);
        astate->getPathFormula().addIntegerConstraint(res_av);
        astate->getPathFormula().addIntegerConstraint(lhs_av);
        astate->getPathFormula().addIntegerConstraint(rhs_av);

        std::string op;
        switch (BO->getOpcode()) {
        case llvm::Instruction::Add:
          op = "+";
          break;
        case llvm::Instruction::Sub:
          op = "-";
          break;
        case llvm::Instruction::Mul:
          op = "*";
          break;
        case llvm::Instruction::SDiv:
        case llvm::Instruction::UDiv:
          op = "/";
          break;
        case llvm::Instruction::SRem:
        case llvm::Instruction::URem:
          op = "%";
          break;
        case llvm::Instruction::And:
          op = "&";
          break;
        case llvm::Instruction::Or:
          op = "|";
          break;
        case llvm::Instruction::Xor:
          op = "^";
          break;
        case llvm::Instruction::Shl:
          op = "<<";
          break;
        case llvm::Instruction::LShr:
        case llvm::Instruction::AShr:
          op = ">>";
          break;
        default:
          break;
        }
        if (!op.empty()) {
          (void)astate->getPathFormula().addArithmeticOperation(res_av, lhs_av,
                                                               rhs_av, op);
        }
      }
    }
    return pruneStates({exec_state});
  }

  // Handle comparisons for path conditions. Skip when used as branch condition:
  // we fork and apply per-branch in applyBranchCondition.
    if (llvm::isa<llvm::ICmpInst>(I) || llvm::isa<llvm::FCmpInst>(I)) {
    const llvm::Instruction *next = I->getNextNode();
    if (auto *BI = next ? llvm::dyn_cast<llvm::BranchInst>(
                              const_cast<llvm::Instruction *>(next))
                        : nullptr) {
      if (BI->isConditional() &&
          BI->getCondition() ==
              static_cast<llvm::Value *>(const_cast<llvm::Instruction *>(I)))
        return {exec_state};
    }
    return pruneStates({handleComparison(I, exec_state, pred)});
  }

  return pruneStates({exec_state});
}

std::vector<ExecutionDomain>
PulseChecker::handleLibraryCall(const llvm::CallInst *CI,
                                ExecutionDomain exec_state,
                                const llvm::BasicBlock *pred) {

  // Delegate to modular models
  auto result = models_->dispatch(CI, exec_state, pred);
  if (result.handled) {
    return result.states;
  }

  return {};
}

ExecutionDomain PulseChecker::handleLoad(const llvm::LoadInst *LI,
                                         ExecutionDomain exec_state,
                                         const llvm::BasicBlock *pred) {
  auto *astate = exec_state.getAstate();
  const llvm::Value *ptr_operand = LI->getPointerOperand();

  // Check if this is a direct load from a stack variable (alloca)
  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(ptr_operand)) {
    // Direct load from alloca - read from stack
    AbstractValue stack_var = factory_.getOrCreate(AI);
    AbstractValue canon_var = astate->getCanonical(stack_var);

    // Check if variable is uninitialized (only for reads, not writes)
	    if (astate->getPostAttrs().has(canon_var, Attribute::Uninitialized)) {
	      Trace trace;
	      trace.addEvent(LI, "Load from uninitialized variable");
	      if (LatentIssue::isManifest(OperationResult::UninitializedRead, *astate,
	                                  canon_var)) {
	        reportBug(OperationResult::UninitializedRead, LI, canon_var, trace,
	                  astate);
	        return ExecutionDomain::abortProgram(
	            std::make_unique<AbductiveDomain>(astate->clone()),
            OperationResult::UninitializedRead, std::move(trace));
      } else {
        latent_issues_.emplace_back(OperationResult::UninitializedRead,
                                    LatentIssue::issueKindFromResult(
                                        OperationResult::UninitializedRead),
                                    canon_var, LI, std::move(trace));
        return ExecutionDomain::latentAbortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            &latent_issues_.back());
      }
    }

    // Variable is initialized - get value from stack
    if (auto *stack_addr = astate->getPostStack().find(AI)) {
      astate->getPostStack().add(LI, *stack_addr);
    } else {
      // Variable not in stack yet - create fresh value (shouldn't happen for
      // initialized)
      AbstractValue fresh = factory_.createFresh(LI);
      Address addr(fresh);
      astate->getPostStack().add(LI, addr);
    }
    return exec_state;
  }

  // Check if pointer operand is in stack map (indirect stack variable)
  if (auto *stack_addr = astate->getPostStack().find(ptr_operand)) {
    // Load from stack variable - check if uninitialized
    AbstractValue canon_ptr = astate->getCanonical(stack_addr->addr);
    AbstractValue loaded_ptr = canon_ptr;

    // PRIORITY ORDER: Invalid > Null > Uninitialized
    // Check Invalid FIRST (UseAfterFree is most severe)
	    if (astate->getPostAttrs().has(loaded_ptr, Attribute::Invalid)) {
	      Trace trace = Trace::fromValueHistory(stack_addr->history);
	      trace.addEvent(LI, "Load of invalid pointer");
	      if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
	                                  loaded_ptr)) {
	        reportBug(OperationResult::UseAfterFree, LI, loaded_ptr, trace, astate);
	        return ExecutionDomain::abortProgram(
	            std::make_unique<AbductiveDomain>(astate->clone()),
            OperationResult::UseAfterFree, std::move(trace));
      } else {
        latent_issues_.emplace_back(
            OperationResult::UseAfterFree,
            LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
            loaded_ptr, LI, std::move(trace));
        return ExecutionDomain::latentAbortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            &latent_issues_.back());
      }
    }

    // Check for null pointer (only if not invalid)
    // DON'T report NullDereference if Invalid is present (UseAfterFree takes
    // priority) NPD checker: only report if source is a null constant
    if (!astate->getPostAttrs().has(loaded_ptr, Attribute::Invalid)) {
      if (astate->getPathFormula().isNull(loaded_ptr) ||
          (astate->getPostAttrs().has(loaded_ptr, Attribute::Null) &&
           !astate->getPathFormula().isNonNull(loaded_ptr))) {
        // Check if this pointer originated from a null constant
        // Check both the stack address and the loaded pointer's AbstractValue
        // source
        bool is_null_source =
            PulseOperations::isNullConstantSource(*stack_addr);
        if (!is_null_source) {
          // Also check if the AbstractValue itself has a null constant source
          Address loaded_addr(loaded_ptr);
          loaded_addr.history = stack_addr->history;
          is_null_source = PulseOperations::isNullConstantSource(loaded_addr);
        }
	        if (is_null_source) {
	          Trace trace = Trace::fromValueHistory(stack_addr->history);
	          trace.addEvent(LI, "Load of null pointer");
	          if (LatentIssue::isManifest(OperationResult::NullDereference, *astate,
	                                      loaded_ptr)) {
	            reportBug(OperationResult::NullDereference, LI, loaded_ptr, trace,
	                      astate);
	            return ExecutionDomain::abortProgram(
                std::make_unique<AbductiveDomain>(astate->clone()),
                OperationResult::NullDereference, std::move(trace));
          } else {
            latent_issues_.emplace_back(OperationResult::NullDereference,
                                        LatentIssue::issueKindFromResult(
                                            OperationResult::NullDereference),
                                        loaded_ptr, LI, std::move(trace));
            return ExecutionDomain::latentAbortProgram(
                std::make_unique<AbductiveDomain>(astate->clone()),
                &latent_issues_.back());
          }
        }
      }
    }

    // Variable is initialized - use value from stack
    astate->getPostStack().add(LI, *stack_addr);
    return exec_state;
  }

  // Heap load (pointer dereference) - use readDeref
  auto ptr_opt = ops_.eval(*astate, ptr_operand, LI, pred);
  if (!ptr_opt) {
    // Eval failed - could be null/invalid pointer in GEP or other operation
    // Check if ptr_operand is a GEP that failed due to null/invalid base
    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr_operand)) {
      auto base_opt = ops_.eval(*astate, GEP->getPointerOperand(), LI, pred);
      if (base_opt) {
        AbstractValue base_canon = astate->getCanonical(base_opt->addr);
        // PRIORITY: Check Invalid FIRST (UseAfterFree > NullDereference)
        if (astate->getPostAttrs().has(base_canon, Attribute::Invalid)) {
          Trace trace = Trace::fromValueHistory(base_opt->history);
          trace.addEvent(LI, "Array access through invalid pointer");
          reportBug(OperationResult::UseAfterFree, LI, base_canon, trace,
                    astate);
          return ExecutionDomain::abortProgram(
              std::make_unique<AbductiveDomain>(astate->clone()),
              OperationResult::UseAfterFree, std::move(trace));
        }
        // Check if base is null (only if not invalid)
        // DON'T report NullDereference if Invalid is present (UseAfterFree
        // takes priority) NPD checker: only report if source is a null constant
        if (!astate->getPostAttrs().has(base_canon, Attribute::Invalid)) {
          if (astate->getPathFormula().isNull(base_canon) ||
              (astate->getPostAttrs().has(base_canon, Attribute::Null) &&
               !astate->getPathFormula().isNonNull(base_canon))) {
            // Check if this pointer originated from a null constant
            if (PulseOperations::isNullConstantSource(*base_opt)) {
              Trace trace = Trace::fromValueHistory(base_opt->history);
              trace.addEvent(LI, "Array access through null pointer");
              reportBug(OperationResult::NullDereference, LI, base_canon, trace,
                        astate);
              return ExecutionDomain::abortProgram(
                  std::make_unique<AbductiveDomain>(astate->clone()),
                  OperationResult::NullDereference, std::move(trace));
            }
          }
        }
      }
    }
    return exec_state;
  }
  // PRIORITY: Check Invalid FIRST before dereferencing (UseAfterFree >
  // NullDereference)
  AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
	  if (astate->getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
	    Trace trace = Trace::fromValueHistory(ptr_opt->history);
	    trace.addEvent(LI, "Dereference of invalid pointer");
	    if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
	                                canon_ptr)) {
	      reportBug(OperationResult::UseAfterFree, LI, canon_ptr, trace, astate);
	      return ExecutionDomain::abortProgram(
	          std::make_unique<AbductiveDomain>(astate->clone()),
          OperationResult::UseAfterFree, std::move(trace));
    } else {
      latent_issues_.emplace_back(
          OperationResult::UseAfterFree,
          LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
          canon_ptr, LI, std::move(trace));
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back());
    }
  }

  auto read_result = ops_.readDeref(*astate, *ptr_opt, LI);
  OperationResult result = read_result.first;
  llvm::Optional<Address> value_opt = read_result.second;

	  if (result != OperationResult::Success) {
	    Trace trace = Trace::fromValueHistory(ptr_opt->history);
	    trace.addEvent(LI, "Load from invalid address");

	    if (LatentIssue::isManifest(result, *astate,
	                                astate->getCanonical(ptr_opt->addr))) {
	      // Manifest error - report immediately
	      reportBug(result, LI, ptr_opt->addr, trace, astate);
	      return ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()), result,
          std::move(trace));
    } else {
      // Latent error - create latent issue
      latent_issues_.emplace_back(result,
                                  LatentIssue::issueKindFromResult(result),
                                  ptr_opt->addr, LI, std::move(trace));
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back());
    }
  }
  if (value_opt) {
    astate->getPostStack().add(LI, *value_opt);

    // Check for taint sink: if loaded value flows to a sink
    // (This would be checked at sink locations, not here)
  }
  return exec_state;
}

ExecutionDomain PulseChecker::handleStore(const llvm::StoreInst *SI,
                                          ExecutionDomain exec_state,
                                          const llvm::BasicBlock *pred) {
  auto *astate = exec_state.getAstate();
  auto value_opt = ops_.eval(*astate, SI->getValueOperand(), SI, pred);
  if (!value_opt)
    return exec_state;

  const llvm::Value *ptr_operand = SI->getPointerOperand();

  // Sound incorrectness: report stack address escape only when provable.
  // If a stack-derived pointer is stored into heap/global memory, it escapes.
  auto maybeReportStackEscape = [&](const Address& stored_value,
                                    const Address& dest_ptr) {
    if (!SI->getValueOperand()->getType()->isPointerTy())
      return;
    AbstractValue canon_value = astate->getCanonical(stored_value.addr);
    if (!astate->getPostAttrs().has(canon_value, Attribute::Stack))
      return;
    AbstractValue canon_dest = astate->getCanonical(dest_ptr.addr);
    const bool dest_is_global =
        astate->getPostAttrs().has(canon_dest, Attribute::Global);
    const bool dest_is_heap =
        astate->getPostAttrs().has(canon_dest, Attribute::Allocated) &&
        !astate->getPostAttrs().has(canon_dest, Attribute::Stack);
    if (!dest_is_global && !dest_is_heap)
      return; // Cannot prove escape.

    Trace trace = Trace::fromValueHistory(stored_value.history);
    trace.addEvent(SI, "Storing stack-derived address into non-stack memory");
    auto diag = std::make_unique<StackVariableAddressEscape>(
        SI, canon_value, "Stack address escapes via store",
        "Do not store addresses of local variables into heap/global memory.",
        std::move(trace));
    DiagnosticManager::getInstance().report(std::move(diag));
  };

  // Check if this is a direct store to a stack variable (alloca)
  // Stack variables are in the stack map, and we can store to them directly
  // without checking for uninitialized (storing initializes them)
  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(ptr_operand)) {
    // Direct store to alloca - update stack and clear Uninitialized
    AbstractValue stack_var = factory_.getOrCreate(AI);
    AbstractValue canon_var = astate->getCanonical(stack_var);

    // Clear Uninitialized attribute (storing initializes the variable)
    astate->getPostAttrs().remove(canon_var, Attribute::Uninitialized);

    // When storing a pointer value, preserve its attributes (Null, Invalid,
    // Allocated)
    [[maybe_unused]] AbstractValue canon_value =
        astate->getCanonical(value_opt->addr);
    if (astate->getPostAttrs().has(canon_value, Attribute::Null)) {
      // Preserve null attribute on the stored value
      astate->getPostAttrs().add(canon_value, Attribute::Null);
    }
    if (astate->getPostAttrs().has(canon_value, Attribute::Invalid)) {
      // Preserve invalid attribute on the stored value
      astate->getPostAttrs().add(canon_value, Attribute::Invalid);
    }
    if (astate->getPostAttrs().has(canon_value, Attribute::Allocated)) {
      // Preserve allocated attribute on the stored value
      astate->getPostAttrs().add(canon_value, Attribute::Allocated);
    }

    // Update stack with new value
    // Add Store event to history so isNullConstantSource can detect null
    // constants stored via CallInst
    Address stored_addr = *value_opt;
    stored_addr.history.addEvent(ValueHistory::EventKind::Store, SI,
                                 SI->getFunction());
    astate->getPostStack().add(AI, stored_addr);

    // Only record copy if it's not a pointer type (to reduce false positives)
    if (!SI->getValueOperand()->getType()->isPointerTy()) {
      analysis_non_disj_.recordCopy(SI);
    }
    return exec_state;
  }

  // Check if pointer operand is in stack map (indirect stack variable)
  if (astate->getPostStack().find(ptr_operand)) {
    // Store to stack variable - clear Uninitialized and update
    auto ptr_opt = ops_.eval(*astate, ptr_operand, SI, pred);
    if (ptr_opt) {
      AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
      astate->getPostAttrs().remove(canon_ptr, Attribute::Uninitialized);

      astate->getPostStack().add(ptr_operand, *value_opt);
    }
    // Only record copy if it's not a pointer type
    if (!SI->getValueOperand()->getType()->isPointerTy()) {
      analysis_non_disj_.recordCopy(SI);
    }
    return exec_state;
  }

  // Heap store (pointer dereference) - use writeDeref
  auto ptr_opt = ops_.eval(*astate, ptr_operand, SI, pred);
  if (!ptr_opt)
    return exec_state;

  maybeReportStackEscape(*value_opt, *ptr_opt);

  auto res = ops_.writeDeref(*astate, *ptr_opt, *value_opt, SI);
  if (res != OperationResult::Success) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(SI, "Store to invalid address");

	    if (LatentIssue::isManifest(res, *astate,
	                                astate->getCanonical(ptr_opt->addr))) {
	      // Manifest error - report immediately
	      reportBug(res, SI, ptr_opt->addr, trace, astate);
	      return ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()), res,
          std::move(trace));
    } else {
      // Latent error - create latent issue
      latent_issues_.emplace_back(res, LatentIssue::issueKindFromResult(res),
                                  ptr_opt->addr, SI, std::move(trace));
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back());
    }
  }
  // Only record copy if it's not a pointer type (to reduce false positives)
  if (!SI->getValueOperand()->getType()->isPointerTy()) {
    analysis_non_disj_.recordCopy(SI);
  }
  return exec_state;
}

std::vector<ExecutionDomain>
PulseChecker::handleCall(const llvm::CallInst *CI, ExecutionDomain exec_state,
                         const llvm::BasicBlock *pred, unsigned call_depth) {
  auto *astate = exec_state.getAstate();
  if (!astate) {
    return {exec_state};
  }

  // Handle LLVM lifetime intrinsics: model end as definite invalidation.
  if (auto *II =
          llvm::dyn_cast<llvm::IntrinsicInst>(const_cast<llvm::CallInst *>(CI))) {
    const auto iid = II->getIntrinsicID();
    if (iid == llvm::Intrinsic::lifetime_start ||
        iid == llvm::Intrinsic::lifetime_end) {
      if (CI->arg_size() >= 2) {
        auto ptr_opt = ops_.eval(*astate, CI->getArgOperand(1), CI, pred);
        if (ptr_opt) {
          AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
          if (iid == llvm::Intrinsic::lifetime_start) {
            // Treat as beginning of lifetime: clear invalidation and mark
            // uninitialized.
            astate->getPostAttrs().remove(canon_ptr, Attribute::Invalid);
            astate->getPostAttrs().add(canon_ptr, Attribute::Uninitialized);
          } else {
            // End of lifetime: uses after this are invalid (gone out of scope).
            ops_.invalidate(*astate, *ptr_opt, CI,
                            InvalidationKind::GoneOutOfScope);
          }
        }
      }
      return {exec_state};
    }
  }

  llvm::Function *F = CI->getCalledFunction();
  if (!F)
    return {exec_state};

  // Check for taint sources/sinks/sanitizers in function calls
  std::string func_name = F->getName().str();
  if (models_->isTaintSource(func_name)) {
    // Mark return value as tainted
    AbstractValue ret_val = factory_.createFresh(CI);
    TaintKind kind = TaintKind::UserInput();
    if (func_name == "recv" || func_name == "recvfrom" ||
        func_name == "recvmsg") {
      kind = TaintKind::Network();
    } else if (func_name == "getenv") {
      kind = TaintKind::Environment();
    } else if (func_name == "getcwd") {
      kind = TaintKind::FileSystem();
    }
    TaintOperations::taint(*astate, ret_val, kind, CI);
    astate->getPostStack().add(CI, Address(ret_val));
    return {exec_state};
  }

  if (models_->isTaintSink(func_name)) {
    // Check if arguments are tainted
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      auto arg_opt = ops_.eval(*astate, CI->getArgOperand(i), CI, pred);
      if (arg_opt) {
        AbstractValue canon_arg = astate->getCanonical(arg_opt->addr);
        TaintOperations::checkSink(*astate, canon_arg, func_name, CI);
      }
    }
  }

  if (models_->isTaintSanitizer(func_name)) {
    // Sanitize arguments (remove taint)
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      auto arg_opt = ops_.eval(*astate, CI->getArgOperand(i), CI, pred);
      if (arg_opt) {
        AbstractValue canon_arg = astate->getCanonical(arg_opt->addr);
        TaintKind sanitizer_kind = TaintKind::Unknown(); // Generic sanitizer
        TaintOperations::sanitize(*astate, canon_arg, sanitizer_kind, CI);
      }
    }
  }

  // Try library models first
  auto lib_result = handleLibraryCall(CI, exec_state, pred);
  if (!lib_result.empty()) {
    return lib_result; // Library model handled it
  }

  // Fall back to old handling for compatibility
  if (F->getName() == "malloc" || F->getName() == "calloc" ||
      F->getName() == "realloc") {
    AbstractValue av = factory_.createFresh(CI);
    ops_.allocate(*astate, av, CI);
    astate->getPostStack().add(CI, Address(av));
    return {exec_state};
  }
  if (F->getName() == "free") {
    if (CI->arg_size() > 0) {
      auto ptr_opt = ops_.eval(*astate, CI->getArgOperand(0), CI, pred);
      if (ptr_opt) {
        ops_.invalidate(*astate, *ptr_opt, CI, InvalidationKind::CFree);

        // Also invalidate aliases
        AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
        for (auto &stack_kv : astate->getPostStack().getMap()) {
          AbstractValue stack_canon =
              astate->getCanonical(stack_kv.second.addr);
          if (stack_canon == canon_ptr) {
            astate->getPostAttrs().add(stack_canon, Attribute::Invalid);
            astate->getPostAttrs().remove(stack_canon, Attribute::Allocated);
          }
        }
      }
    }
    return {exec_state};
  }

  // Calls within the current SCC are treated as unknown to avoid
  // order-dependent interprocedural behavior in recursive cycles.
  if (current_scc_.count(F) > 0) {
    astate->addSkippedCall(F->getName().str());
    astate->declareUnknownValues();
    if (CI->getType()->isPointerTy()) {
      AbstractValue ret_val = factory_.createFresh(CI);
      Address ret_addr(ret_val);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      astate->getPostStack().add(CI, ret_addr);
    }
    return {exec_state};
  }

  if (F->isDeclaration()) {
    // External function with no model - record as skipped
    astate->addSkippedCall(F->getName().str());
    astate->declareUnknownValues();
    if (CI->getType()->isPointerTy()) {
      AbstractValue ret_val = factory_.createFresh(CI);
      Address ret_addr(ret_val);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      astate->getPostStack().add(CI, ret_addr);
    }
    return {exec_state};
  }

  // Try to use summary if available
  if (summary_manager_.hasSummary(F)) {
    PulseLogger::debug("Applying summary for " + F->getName().str());
    PulseLogger::incrementCounter("summaries.applied");
    auto summary_results = applySummaryImproved(F, exec_state, CI, pred);
    if (!summary_results.empty()) {
      return summary_results;
    }
    // Fall back to legacy summary application if the improved engine cannot
    // apply any entry.
    auto legacy_results = applySummary(F, exec_state, CI, pred);
    if (!legacy_results.empty()) {
      return legacy_results;
    }
  }

  // No summary could be applied. Conservatively treat the call as unknown.
  astate->addSkippedCall(F->getName().str());
  astate->declareUnknownValues();
  if (CI->getType()->isPointerTy()) {
    AbstractValue ret_val = factory_.createFresh(CI);
    Address ret_addr(ret_val);
    ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                              CI->getFunction());
    astate->getPostStack().add(CI, ret_addr);
  }
  return {exec_state};
}

ExecutionDomain PulseChecker::handleAlloca(const llvm::AllocaInst *AI,
                                           ExecutionDomain exec_state) {
  auto *astate = exec_state.getAstate();
  AbstractValue av = factory_.getOrCreate(AI);
  ops_.allocate(*astate, av, AI);
  // Stack allocation: mark as stack-derived so we can prove invalid escapes.
  astate->getPostAttrs().add(av, Attribute::Stack);
  astate->getPostAttrs().add(av, Attribute::Uninitialized);
  astate->getPostStack().add(AI, Address(av));
  return exec_state;
}

ExecutionDomain PulseChecker::handleReturn(const llvm::ReturnInst *RI,
                                           ExecutionDomain exec_state) {
  auto *astate = exec_state.getAstate();
  if (RI && astate && RI->getNumOperands() > 0) {
    const llvm::Value *ret_v = RI->getReturnValue();
    if (ret_v && ret_v->getType()->isPointerTy()) {
      auto ret_opt = ops_.eval(*astate, ret_v, RI, nullptr);
      if (ret_opt) {
        AbstractValue canon_ret = astate->getCanonical(ret_opt->addr);
        if (astate->getPostAttrs().has(canon_ret, Attribute::Stack)) {
          Trace trace = Trace::fromValueHistory(ret_opt->history);
          trace.addEvent(RI, "Returning address derived from stack allocation");
          auto diag = std::make_unique<StackVariableAddressEscape>(
              RI, canon_ret, "Stack address escapes via return",
              "Do not return addresses of local variables; allocate on heap or return by value.",
              std::move(trace));
          DiagnosticManager::getInstance().report(std::move(diag));
        }
      }
    }
  }
  // Convert to ExitProgram variant
  if (exec_state.isContinueProgram() && exec_state.getAstate()) {
    return ExecutionDomain::exitProgram(
        std::make_unique<AbductiveDomain>(exec_state.getAstate()->clone()));
  }
  return exec_state;
}

std::vector<std::pair<ExecutionDomain, llvm::Optional<AbstractValue>>>
PulseChecker::runCallee(const llvm::Function *callee,
                        const ExecutionDomain &caller_state,
                        const llvm::CallInst *CI, const llvm::BasicBlock *pred,
                        unsigned call_depth) {
  std::vector<std::pair<ExecutionDomain, llvm::Optional<AbstractValue>>> result;
  if (call_depth >= kMaxCallDepth)
    return result;

  ExecutionDomain init = initializeFunction(callee);
  auto *init_astate = init.getAstate();
  ExecutionDomain caller_eval_state = caller_state.clone();
  auto *caller_eval_astate = caller_eval_state.getAstate();
  if (!init_astate || !caller_eval_astate)
    return result;

  const auto *ai = callee->arg_begin();
  const auto *ae = callee->arg_end();
  unsigned i = 0;
  for (; ai != ae && i < CI->arg_size(); ++ai, ++i) {
    if (!ai->getType()->isPointerTy())
      continue;
    // Evaluate actuals in the caller's state (not in the fresh callee state).
    auto opt = ops_.eval(*caller_eval_astate, CI->getArgOperand(i), CI, pred);
    if (opt) {
      init_astate->getPostStack().add(&*ai, *opt);
      AbstractValue formal_av = factory_.getOrCreate(&*ai);
      init_astate->getPostAttrs().remove(formal_av, Attribute::Uninitialized);
    }
  }

  // Use block-based worklist for proper CFG traversal
  std::map<const llvm::BasicBlock *, std::vector<ExecutionDomain>>
      block_entry_states;
  block_entry_states[&callee->getEntryBlock()].push_back(std::move(init));

  std::queue<const llvm::BasicBlock *> worklist;
  worklist.push(&callee->getEntryBlock());
  std::set<const llvm::BasicBlock *> processed;

  unsigned iter_limit = 0;
  const unsigned max_iter = 50000;

  while (!worklist.empty() && iter_limit++ < max_iter) {
    const llvm::BasicBlock *BB = worklist.front();
    worklist.pop();

    if (processed.count(BB) && block_entry_states[BB].empty())
      continue;

    std::vector<ExecutionDomain> entry_states =
        std::move(block_entry_states[BB]);
    block_entry_states[BB].clear();

    if (entry_states.empty())
      continue;

    // Join states at block entry.
    //
    // Sound incorrectness: avoid over-approximating joins when multiple
    // disjunctive entry states arrive at the same block. A union-style merge can
    // fabricate heap facts and admit non-witnessable bug paths. Prefer keeping a
    // representative witness state.
    ExecutionDomain block_state;
    if (entry_states.size() == 1) {
      block_state = std::move(entry_states[0]);
    } else {
      block_state = std::move(entry_states[0]);
    }

    if (block_state.isStopped())
      continue;

    // Process instructions in block
    ExecutionDomain current_state = std::move(block_state);

    for (const llvm::Instruction &I : *BB) {
      if (current_state.isStopped())
        break;

      if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
        llvm::Optional<AbstractValue> ret_av;
        if (RI->getNumOperands() > 0 &&
            RI->getReturnValue()->getType()->isPointerTy()) {
          auto *a = current_state.getAstate();
          if (a) {
            auto opt = ops_.eval(*a, RI->getReturnValue(), RI, nullptr);
            if (opt)
              ret_av = opt->addr;
          }
        }
        result.push_back({current_state.clone(), ret_av});
        continue;
      }

      const llvm::BasicBlock *pred_bb = nullptr;
      if (llvm::isa<llvm::PHINode>(&I)) {
        if (pred_begin(BB) != pred_end(BB)) {
          pred_bb = *pred_begin(BB);
        }
      }

      auto new_states =
          executeInstruction(&I, current_state, pred_bb, call_depth + 1);
      if (new_states.empty() || new_states[0].isStopped()) {
        current_state =
            new_states.empty() ? current_state : std::move(new_states[0]);
        break;
      }
      current_state = std::move(new_states[0]);
    }

    // Propagate to successors
    if (!current_state.isStopped() && BB->getTerminator()) {
      for (const llvm::BasicBlock *succ : llvm::successors(BB)) {
        if (succ->empty())
          continue;
        block_entry_states[succ].push_back(current_state.clone());
        if (processed.find(succ) == processed.end() ||
            !block_entry_states[succ].empty()) {
          worklist.push(succ);
        }
      }
    }

    processed.insert(BB);

    if (worklist.size() > kMaxDisjuncts) {
      while (worklist.size() > kMaxDisjuncts) {
        worklist.pop();
      }
    }
  }

  return result;
}

void PulseChecker::reportBug(OperationResult kind, const llvm::Instruction *loc,
                             AbstractValue addr, const Trace &trace,
                             const AbductiveDomain *astate) {
  PulseLogger::logBug(kind, loc);
  PulseLogger::incrementCounter("bugs.total");

  std::unique_ptr<Diagnostic> diagnostic;

  switch (kind) {
  case OperationResult::UseAfterFree: {
    InvalidationKind invKind = InvalidationKind::Other;
    if (astate) {
      AbstractValue canon = astate->getCanonical(addr);
      auto inv = astate->getInvalidationInfo(canon);
      if (inv)
        invKind = inv->first;
    }
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Use after free detected", "Access to freed memory",
        "Ensure the memory is not freed before access", IssueType::UseAfterFree,
        trace.clone(), invKind);
    break;
  }
  case OperationResult::NullDereference: {
    // For NPD checker, only show null constant sources in the trace
    Trace filtered_trace;
    for (const auto &event : trace.getEvents()) {
      if (!event.location)
        continue;
      // Only include Store events where a null constant is stored
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(event.location)) {
        const llvm::Value *stored_value = SI->getValueOperand();
        // Check if storing a null pointer constant (not integer 0).
        if (isNullPointerConstantValue(stored_value)) {
          filtered_trace.addEvent(event.location, event.function,
                                  "Null constant stored");
        }
      }
    }
    // Always add the dereference location as the sink
    filtered_trace.addEvent(loc, loc ? loc->getFunction() : nullptr,
                            "Null pointer dereference");
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Null pointer dereference", "Pointer is null",
        "Check for null before dereferencing", IssueType::NullDereference,
        std::move(filtered_trace));
    break;
  }
  case OperationResult::UninitializedRead:
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Uninitialized read", "Reading uninitialized memory",
        "Initialize variable before use", IssueType::UninitializedRead,
        trace.clone());
    break;
  case OperationResult::TaintError:
    diagnostic = std::make_unique<TaintFlow>(loc, "Unknown Source",
                                             "Unknown Sink", trace.clone());
    break;
  default:
    return;
  }

  if (diagnostic) {
    DiagnosticManager::getInstance().report(std::move(diagnostic));
  }
}

void PulseChecker::reportUnnecessaryCopies(const llvm::Function *F) {
  (void)F;
  const auto &stores = analysis_non_disj_.getCopiedStores();
  for (const llvm::StoreInst *SI : stores) {
    // Skip reporting unnecessary copies for pointer types (reduce false
    // positives)
    if (SI->getValueOperand()->getType()->isPointerTy())
      continue;

    // Skip if storing a constant (not really a copy)
    if (llvm::isa<llvm::Constant>(SI->getValueOperand()))
      continue;

    // Skip if storing to/from alloca (local variable initialization)
    if (llvm::isa<llvm::AllocaInst>(SI->getPointerOperand()))
      continue;

    // Skip if this is a PHI node result (common in SSA form)
    if (llvm::isa<llvm::PHINode>(SI->getValueOperand()))
      continue;

    // Create Diagnostic for unnecessary copy
    // For now, assume variable name extraction from IR
    std::string varName = "variable";
    std::string typeName = "type";
    auto diag = std::make_unique<UnnecessaryCopy>(
        static_cast<const llvm::Instruction *>(SI), varName, typeName);
    DiagnosticManager::getInstance().report(std::move(diag));
  }
}

void PulseChecker::reportConstRefableParams(const llvm::Function *F) {
  (void)F;
  for (const llvm::Argument *A : analysis_non_disj_.getConstRefableParams()) {
    const llvm::Instruction *firstUse =
        A->getParent()->getEntryBlock().getFirstNonPHI();
    if (!firstUse)
      continue;

    // This is a special case not fully covered by PulseDiagnostic yet in my
    // port, but we can add it or reuse UnnecessaryCopy Or just log it for now
    // TODO: Add ConstRefableParam to PulseDiagnostic
  }
}

void PulseChecker::reportDiagnostic(const llvm::Instruction *loc,
                                    const std::string &message,
                                    const std::string &type, int confidence) {
  (void)confidence;
  // For TaintSink, specialized handling
  if (type == "TaintSink") {
    Trace trace;
    trace.addEvent(loc, message);
    auto diag = std::make_unique<TaintFlow>(loc, "Taint Source", "Taint Sink",
                                            std::move(trace));
    DiagnosticManager::getInstance().report(std::move(diag));
  } else {
    // Generic diagnostic reporting via stderr or log
    // Or create a GenericDiagnostic class
    llvm::errs() << "[Pulse] " << type << ": " << message << " at " << *loc
                 << "\n";
  }
}

void PulseChecker::reportDiagnostic(const Diagnostic &diagnostic) {
  (void)diagnostic;
  // Backward-compatibility overload: structured diagnostics should be
  // constructed and reported via DiagnosticManager.
}

void PulseChecker::createSummary(
    const llvm::Function *F, const std::vector<ExecutionDomain> &exit_states,
    const std::vector<ExecutionDomain> &latent_exit_states) {
  PulseLogger::debug("Creating summary for " + F->getName().str() + " (" +
                     std::to_string(exit_states.size()) + " exit states, " +
                     std::to_string(latent_exit_states.size()) +
                     " latent states)");
  PulseLogger::incrementCounter("summaries.created");
  if (exit_states.empty() && latent_exit_states.empty())
    return;

  auto computeReturnValue =
      [&](const AbductiveDomain &astate) -> llvm::Optional<AbstractValue> {
    for (const auto &BB : *F) {
      if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
        if (RI->getNumOperands() == 0)
          continue;
        const llvm::Value *ret_v = RI->getReturnValue();
        if (!ret_v || !ret_v->getType()->isPointerTy())
          continue;
        const auto *ret_addr = astate.getPostStack().find(ret_v);
        if (!ret_addr)
          continue;
        return astate.getCanonical(ret_addr->addr);
      }
    }
    return llvm::None;
  };

  PulseSummary summary(F);

  unsigned latent_added = 0;
  for (const auto &latent_state : latent_exit_states) {
    if (!latent_state.isStopped())
      continue;
    if (!latent_state.isLatentAbortProgram() &&
        !latent_state.isLatentInvalidAccess())
      continue;
    if (summary.getPrePostList().size() >= kMaxDisjuncts)
      break;
    const AbductiveDomain *astate = latent_state.getAstate();
    if (!astate)
      continue;

    auto *issue = latent_state.getStoppedExecution().latent_issue;
    if (!issue)
      continue;

    SummaryEntry::LatentIssueSummary latent;
    latent.diagnostic = issue->getDiagnostic();
    latent.address = astate->getCanonical(issue->getAddress());
    latent.trace = issue->getTrace().clone();
    latent.calling_context = issue->getCallingContext();

    const PulseFormula formula = astate->getPathFormula().clone();
    auto pre = std::make_unique<AbductiveDomain>(astate->clone());
    auto post = std::make_unique<AbductiveDomain>(astate->clone());
    summary.addPrePost(SummaryEntry(
        std::move(pre), formula.clone(), std::move(post), formula.clone(),
        llvm::None,
        llvm::Optional<SummaryEntry::LatentIssueSummary>(std::move(latent))));
    latent_added++;
  }

  bool has_any_entry = latent_added > 0;
  const unsigned max_normal_entries =
      kMaxDisjuncts > latent_added ? (kMaxDisjuncts - latent_added) : 0u;

  for (const auto &exit_state : exit_states) {
    if (exit_state.isStopped())
      continue;
    const AbductiveDomain *astate = exit_state.getAstate();
    if (!astate)
      continue;

    has_any_entry = true;
    const PulseFormula formula = astate->getPathFormula().clone();
    const llvm::Optional<AbstractValue> ret_val = computeReturnValue(*astate);

    const unsigned normal_entries =
        static_cast<unsigned>(summary.getPrePostList().size()) - latent_added;
    if (normal_entries < max_normal_entries) {
      auto pre = std::make_unique<AbductiveDomain>(astate->clone());
      auto post = std::make_unique<AbductiveDomain>(astate->clone());
      summary.addPrePost(SummaryEntry(std::move(pre), formula.clone(),
                                      std::move(post), formula.clone(),
                                      ret_val));
      continue;
    }
    // Budget exhausted: for sound incorrectness, do not merge multiple exit
    // states into a single summary entry. Union-style merging can fabricate
    // heap facts and admit non-witnessable caller paths. Prefer dropping extra
    // disjuncts (reduces recall, preserves witnessability).
    break;
  }

  if (!has_any_entry)
    return;

  // Store formal parameter mappings
  for (const auto &Arg : F->args()) {
    if (Arg.getType()->isPointerTy()) {
      AbstractValue formal_av = factory_.getOrCreate(&Arg);
      summary.setFormalAV(&Arg, formal_av);
    }
  }

  summary_manager_.storeSummary(F, std::move(summary));
}

std::vector<ExecutionDomain> PulseChecker::applySummary(
    const llvm::Function *callee, const ExecutionDomain &caller_state,
    const llvm::CallInst *CI, const llvm::BasicBlock *pred) {

  const PulseSummary *summary_ptr = summary_manager_.getSummary(callee);
  if (!summary_ptr || !summary_ptr->isValid()) {
    return {};
  }

  const PulseSummary &summary = *summary_ptr;
  std::vector<ExecutionDomain> results;

  const AbductiveDomain *caller_astate = caller_state.getAstate();
  if (!caller_astate)
    return {caller_state};

  // Create new state by applying summary
  ExecutionDomain new_state = caller_state.clone();
  auto *new_astate = new_state.getAstate();
  if (!new_astate)
    return {caller_state};

  // Build substitution: map formal parameters to actual arguments
  Substitution substitution;

  const auto *pre = summary.getPre();
  const auto *post = summary.getPost();

  unsigned arg_idx = 0;
  for (const auto &Arg : callee->args()) {
    if (arg_idx >= CI->arg_size())
      break;
    if (!Arg.getType()->isPointerTy()) {
      arg_idx++;
      continue;
    }

    // Get actual argument value (use new_astate which is non-const)
    auto actual_opt =
        ops_.eval(*new_astate, CI->getArgOperand(arg_idx), CI, pred);
    if (actual_opt) {
      // Get formal abstract value from summary
      auto formal_av_opt = summary.getFormalAV(&Arg);
      if (formal_av_opt) {
        // Map formal to actual (canonicalize both)
        AbstractValue formal_canon = pre->getCanonical(*formal_av_opt);
        AbstractValue actual_canon = new_astate->getCanonical(actual_opt->addr);
        substitution.add(formal_canon, actual_canon);
      }
    }
    arg_idx++;
  }

  // Check for contradictions: verify pre-condition is satisfied
  // Check if actual arguments satisfy the pre-condition's constraints
  bool has_contradiction = false;
  for (const auto &kv : pre->getPreStack().getMap()) {
    auto formal_av_opt = summary.getFormalAV(kv.first);
    if (!formal_av_opt)
      continue;

    AbstractValue formal_canon = pre->getCanonical(*formal_av_opt);
    auto actual_opt = substitution.substitute(formal_canon);
    if (!actual_opt)
      continue;

    // Check if pre-condition attributes are satisfied
    const auto &pre_attrs = pre->getPreAttrs().get(*formal_av_opt);
    for (Attribute attr : pre_attrs) {
      if (attr == Attribute::Null) {
        // Pre-condition says formal is null, but actual might not be
        if (!new_astate->getPathFormula().isNull(*actual_opt) &&
            !new_astate->getPostAttrs().has(*actual_opt, Attribute::Null)) {
          // Contradiction: pre says null but actual is not null
          has_contradiction = true;
          break;
        }
      } else if (attr == Attribute::Allocated) {
        // Pre-condition says formal is allocated
        if (new_astate->getPostAttrs().has(*actual_opt, Attribute::Invalid)) {
          // Contradiction: pre says allocated but actual is invalid
          has_contradiction = true;
          break;
        }
      }
    }
    if (has_contradiction)
      break;
  }

  // Check path condition contradictions
  if (!has_contradiction) {
    // Merge caller's path condition with callee's pre-formula (after
    // substitution)
    PulseFormula caller_formula = new_astate->getPathFormula().clone();
    PulseFormula callee_pre_formula = summary.getPreFormula();

    // Apply substitution to callee's pre-formula constraints
    // For now, we do a simplified check: if caller has constraints that
    // contradict the substituted pre-formula, we have a contradiction (Full
    // implementation would substitute all values in the formula)

    // Simple check: if pre-formula has null constraints that contradict caller
    for (const auto &kv : pre->getPreStack().getMap()) {
      auto formal_av_opt = summary.getFormalAV(kv.first);
      if (!formal_av_opt)
        continue;

      AbstractValue formal_canon = pre->getCanonical(*formal_av_opt);
      auto actual_opt = substitution.substitute(formal_canon);
      if (!actual_opt)
        continue;

      // Check null/non-null contradictions
      if (callee_pre_formula.isNull(formal_canon)) {
        if (caller_formula.isNonNull(*actual_opt)) {
          has_contradiction = true;
          break;
        }
      } else if (callee_pre_formula.isNonNull(formal_canon)) {
        if (caller_formula.isNull(*actual_opt)) {
          has_contradiction = true;
          break;
        }
      }
    }
  }

  if (has_contradiction) {
    // Contradiction detected - cannot apply this summary.
    return {};
  }

  // Apply post-condition from summary with substitution
  // Copy post-heap edges (with substitution)
  for (const auto &kv : post->getPostHeap().getEdges()) {
    AbstractValue formal_from = kv.first;
    AbstractValue actual_from = substitution.substituteOrIdentity(formal_from);

    for (const auto &edge_kv : kv.second) {
      const Access &access = edge_kv.first;
      const Address &formal_target = edge_kv.second;

      // Substitute target address
      Address actual_target = applySubstitution(substitution, formal_target);

      // Add edge to caller's heap
      if (!new_astate->getPostHeap().findEdge(actual_from, access)) {
        new_astate->getPostHeap().addEdge(actual_from, access, actual_target);
      }
    }
  }

  // Copy post-attributes (with substitution)
  for (const auto &kv : post->getPostAttrs().getAttrs()) {
    AbstractValue formal_av = kv.first;
    AbstractValue actual_av = substitution.substituteOrIdentity(formal_av);

    for (Attribute attr : kv.second) {
      new_astate->getPostAttrs().add(actual_av, attr);
    }
  }

  // Merge post-formula (with substitution applied conceptually)
  PulseFormula post_formula = summary.getPostFormula();
  // Merge formulas (substitution is handled implicitly through
  // canonicalization)
  PulseFormula merged_formula =
      PulseFormula::merge(new_astate->getPathFormula(), post_formula);
  if (merged_formula.isConsistent()) {
    new_astate->setPathFormula(
        std::make_unique<PulseFormula>(std::move(merged_formula)));
  }

  // Handle return value with substitution
  if (summary.getReturnValue()) {
    AbstractValue formal_ret = *summary.getReturnValue();
    AbstractValue actual_ret = substitution.substituteOrIdentity(formal_ret);

    // If return value was substituted, use it; otherwise create fresh
    if (substitution.substitute(formal_ret)) {
      Address ret_addr(actual_ret);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      new_astate->getPostStack().add(CI, ret_addr);
    } else {
      // Return value not in substitution (fresh value from callee)
      // Check if the function returns a null constant
      const llvm::Function *callee = summary.getFunction();
      const llvm::Value *null_constant_ret = nullptr;

      // Check if any ReturnInst in the function returns a null constant
      for (const auto &BB : *callee) {
        if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
          if (RI->getNumOperands() > 0) {
                  const llvm::Value *ret_val = RI->getReturnValue();
                  if (ret_val && ret_val->getType()->isPointerTy()) {
                    if (isNullPointerConstantValue(ret_val)) {
                      null_constant_ret = ret_val;
                      break;
                    }
                  }
          }
        }
      }

      // Create return value: use null constant as source if function returns
      // null
      AbstractValue fresh_ret;
      if (null_constant_ret) {
        fresh_ret = factory_.createFresh(null_constant_ret);
      } else {
        fresh_ret = factory_.createFresh(CI);
      }

      Address ret_addr(fresh_ret);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      new_astate->getPostStack().add(CI, ret_addr);

      // Copy attributes from formal return to fresh return
      const auto &ret_attrs = post->getPostAttrs().get(formal_ret);
      for (Attribute attr : ret_attrs) {
        new_astate->getPostAttrs().add(fresh_ret, attr);
      }

      // If function returns null constant, also set Null attribute and path
      // formula
      if (null_constant_ret) {
        new_astate->getPostAttrs().add(fresh_ret, Attribute::Null);
        new_astate->getPathFormula().addNull(fresh_ret);
      }
    }
  }

  results.push_back(std::move(new_state));
  return results;
}

ExecutionDomain
PulseChecker::handleComparison(const llvm::Instruction *I,
                               ExecutionDomain exec_state,
                               const llvm::BasicBlock *pred_bb) {
  auto *astate = exec_state.getAstate();
  if (!astate)
    return exec_state;

  if (auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(I)) {
    llvm::ICmpInst::Predicate cmp_pred = ICmp->getPredicate();
    llvm::Value *op0 = ICmp->getOperand(0);
    llvm::Value *op1 = ICmp->getOperand(1);

    // Handle null pointer comparisons
    bool op0_is_null = isNullPointerConstantValue(op0);
    bool op1_is_null = isNullPointerConstantValue(op1);

    if (op0_is_null || op1_is_null) {
      llvm::Value *ptr = op0_is_null ? op1 : op0;
      auto ptr_opt = ops_.eval(*astate, ptr, I, pred_bb);
      if (ptr_opt) {
        AbstractValue ptr_av = astate->getCanonical(ptr_opt->addr);

        // Keep the ordering consistent with the rest of the engine:
        // Invalid (UAF) takes priority over null facts.
        bool is_invalid =
            astate->getPostAttrs().has(ptr_av, Attribute::Invalid);
        if (!is_invalid && cmp_pred == llvm::ICmpInst::ICMP_EQ) {
          // ptr == null: mark as null in path formula only (for path-sensitive
          // analysis) Don't set Null attribute here - only null constants set
          // the Null attribute This ensures NPD checker only tracks null
          // constants as sources
          astate->getPathFormula().addNull(ptr_av);
        } else if (!is_invalid && cmp_pred == llvm::ICmpInst::ICMP_NE) {
          // ptr != null: mark as non-null in formula
          astate->getPathFormula().addNonNull(ptr_av);
          // Don't remove Null attribute here - it should only be set by null
          // constants
        }
      }
    }

    // Also handle comparisons where we check if a pointer is null indirectly
    // e.g., if (ptr) or if (!ptr)
    if (op0->getType()->isPointerTy() && op1->getType()->isPointerTy()) {
      auto ptr0_opt = ops_.eval(*astate, op0, I, pred_bb);
      auto ptr1_opt = ops_.eval(*astate, op1, I, pred_bb);

      if (ptr0_opt && ptr1_opt) {
        AbstractValue av0 = astate->getCanonical(ptr0_opt->addr);
        AbstractValue av1 = astate->getCanonical(ptr1_opt->addr);

        // If comparing with zero constant (null check)
        if (op0_is_null || op1_is_null) {
          AbstractValue ptr_av = op0_is_null ? av1 : av0;
          bool is_invalid =
              astate->getPostAttrs().has(ptr_av, Attribute::Invalid);
          if (!is_invalid && cmp_pred == llvm::ICmpInst::ICMP_EQ) {
            // ptr == null: mark as null in path formula only
            // Don't set Null attribute here - only null constants set the Null
            // attribute
            astate->getPathFormula().addNull(ptr_av);
          } else if (!is_invalid && cmp_pred == llvm::ICmpInst::ICMP_NE) {
            // ptr != null: mark as non-null in formula
            astate->getPathFormula().addNonNull(ptr_av);
            // Don't remove Null attribute here - it should only be set by null
            // constants
          }
        }
      }
    }

    // Handle equality comparisons between pointers
    if (cmp_pred == llvm::ICmpInst::ICMP_EQ && op0->getType()->isPointerTy() &&
        op1->getType()->isPointerTy()) {
      auto av0_opt = ops_.eval(*astate, op0, I, pred_bb);
      auto av1_opt = ops_.eval(*astate, op1, I, pred_bb);
      if (av0_opt && av1_opt) {
        astate->addEquality(av0_opt->addr, av1_opt->addr);
      }
    }

    if (cmp_pred == llvm::ICmpInst::ICMP_NE && op0->getType()->isPointerTy() &&
        op1->getType()->isPointerTy()) {
      auto av0_opt = ops_.eval(*astate, op0, I, pred_bb);
      auto av1_opt = ops_.eval(*astate, op1, I, pred_bb);
      if (av0_opt && av1_opt) {
        astate->getPathFormula().addDisequality(av0_opt->addr, av1_opt->addr);
      }
    }
  }

  return exec_state;
}

llvm::Optional<ExecutionDomain> PulseChecker::applyBranchCondition(
    ExecutionDomain state, const llvm::BranchInst *BI, unsigned successor_index,
    const llvm::BasicBlock *pred_bb) {
  if (!BI->isConditional() || successor_index > 1)
    return llvm::Optional<ExecutionDomain>(std::move(state));
  ExecutionDomain forked = state.clone();
  auto *astate = forked.getAstate();
  if (!astate)
    return llvm::Optional<ExecutionDomain>(std::move(state));
  llvm::Value *cond = BI->getCondition();
  auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(cond);
  if (!ICmp)
    return state;
  llvm::Value *op0 = ICmp->getOperand(0);
  llvm::Value *op1 = ICmp->getOperand(1);
  llvm::ICmpInst::Predicate cmp_pred = ICmp->getPredicate();
  bool op0_null = isNullPointerConstantValue(op0);
  bool op1_null = isNullPointerConstantValue(op1);
  bool is_then = (successor_index == 0);

  if (op0_null || op1_null) {
    llvm::Value *ptr = op0_null ? op1 : op0;
    auto ptr_opt = ops_.eval(*astate, ptr, ICmp, pred_bb);
    if (!ptr_opt)
      return llvm::Optional<ExecutionDomain>(std::move(state));
    AbstractValue ptr_av = ptr_opt->addr;
    AbstractValue canon_ptr_av = astate->getCanonical(ptr_av);

    // PRIORITY: If pointer is already Invalid, skip null path entirely
    // This prevents false positives where we report NullDereference when
    // UseAfterFree is correct
    bool is_invalid =
        astate->getPostAttrs().has(canon_ptr_av, Attribute::Invalid);

    if (is_invalid) {
      // Pointer is invalid - only take the non-null path (where we'll report
      // UseAfterFree) Skip the null path to avoid false positive
      // NullDereference
      if (cmp_pred == llvm::ICmpInst::ICMP_EQ && is_then) {
        // This is the null path (ptr == null) - skip it
        return llvm::None;
      }
      if (cmp_pred == llvm::ICmpInst::ICMP_NE && !is_then) {
        // This is the null path (ptr != null, else branch) - skip it
        return llvm::None;
      }
    }

    if (cmp_pred == llvm::ICmpInst::ICMP_EQ) {
      if (is_then) {
        // If we already have evidence this pointer is allocated (post or pre),
        // then taking the [ptr == 0] branch makes the current path
        // contradictory. Do not silently drop it: record a latent issue so
        // callers like foo(0) can be reported.
        bool is_allocated =
            astate->getPostAttrs().has(canon_ptr_av, Attribute::Allocated) ||
            astate->getPreAttrs().has(canon_ptr_av, Attribute::Allocated);
        if (is_allocated) {
          Trace trace = Trace::fromValueHistory(ptr_opt->history);
          trace.addEvent(ICmp, "Assumed null for allocated pointer");
          latent_issues_.emplace_back(OperationResult::NullDereference,
                                      LatentIssue::IssueKind::NullDereference,
                                      canon_ptr_av, ICmp, trace.clone());
          latent_issues_.back().addCallingContext(ICmp->getFunction(), ICmp);
          return llvm::Optional<ExecutionDomain>(
              ExecutionDomain::latentAbortProgram(
                  std::make_unique<AbductiveDomain>(astate->clone()),
                  &latent_issues_.back()));
        }

        if (!astate->getPathFormula().addNull(canon_ptr_av))
          return llvm::None;
        // Don't set Null attribute here - only null constants set the Null
        // attribute This ensures NPD checker only tracks null constants as
        // sources
      } else {
        astate->getPathFormula().addNonNull(canon_ptr_av);
        // Don't remove Null attribute here - it should only be set by null
        // constants
      }
    } else if (cmp_pred == llvm::ICmpInst::ICMP_NE) {
      if (is_then) {
        astate->getPathFormula().addNonNull(canon_ptr_av);
        // Don't remove Null attribute here - it should only be set by null
        // constants
      } else {
        if (!astate->getPathFormula().addNull(canon_ptr_av))
          return llvm::None;
        // Don't set Null attribute here - only null constants set the Null
        // attribute
      }
    } else {
      return llvm::Optional<ExecutionDomain>(std::move(state));
    }
    return llvm::Optional<ExecutionDomain>(std::move(forked));
  }

  // Integer comparisons contribute to path constraints too.
  if (op0->getType()->isIntegerTy() && op1->getType()->isIntegerTy()) {
    auto a0_opt = ops_.eval(*astate, op0, ICmp, pred_bb);
    auto a1_opt = ops_.eval(*astate, op1, ICmp, pred_bb);
    if (!a0_opt || !a1_opt)
      return llvm::Optional<ExecutionDomain>(std::move(state));
    AbstractValue av0 = astate->getCanonical(a0_opt->addr);
    AbstractValue av1 = astate->getCanonical(a1_opt->addr);

    llvm::ICmpInst::Predicate eff_pred =
        is_then ? cmp_pred : invertIcmpPred(cmp_pred);
    if (!applyIntegerIcmpConstraint(astate->getPathFormula(), eff_pred, av0,
                                    av1)) {
      return llvm::None;
    }
    return llvm::Optional<ExecutionDomain>(std::move(forked));
  }

  if (cmp_pred != llvm::ICmpInst::ICMP_EQ &&
      cmp_pred != llvm::ICmpInst::ICMP_NE)
    return llvm::Optional<ExecutionDomain>(std::move(state));

  if (op0->getType()->isPointerTy() && op1->getType()->isPointerTy()) {
    auto av0_opt = ops_.eval(*astate, op0, ICmp, pred_bb);
    auto av1_opt = ops_.eval(*astate, op1, ICmp, pred_bb);
    if (!av0_opt || !av1_opt)
      return llvm::Optional<ExecutionDomain>(std::move(state));
    AbstractValue av0 = av0_opt->addr;
    AbstractValue av1 = av1_opt->addr;

    bool should_be_equal =
        (cmp_pred == llvm::ICmpInst::ICMP_EQ) ? is_then : !is_then;
    if (should_be_equal) {
      if (!astate->getPathFormula().addEquality(av0, av1))
        return llvm::None;
    } else {
      if (!astate->getPathFormula().addDisequality(av0, av1))
        return llvm::None;
    }
    return llvm::Optional<ExecutionDomain>(std::move(forked));
  }

  return llvm::Optional<ExecutionDomain>(std::move(state));
}

} // namespace pulse
