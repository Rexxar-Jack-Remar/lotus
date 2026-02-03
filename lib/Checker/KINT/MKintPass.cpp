#include "Checker/KINT/MKintPass.h"
#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <z3++.h>

#include <algorithm>
#include <cstdint>

using namespace llvm;

namespace kint {

MKintPass::MKintPass() : m_solver(llvm::None), m_function_timeout(FunctionTimeout) {
    m_range_analysis = std::make_unique<RangeAnalysis>();
    m_taint_analysis = std::make_unique<TaintAnalysis>();
    m_bug_detection = std::make_unique<BugDetection>();
    
    // Register bug types with BugReportMgr (Clearblue pattern)
    BugReportMgr& mgr = BugReportMgr::get_instance();
    m_intOverflowTypeId = mgr.register_bug_type("Integer Overflow", BugDescription::BI_HIGH,
                                                 BugDescription::BC_SECURITY, "CWE-190");
    m_divByZeroTypeId = mgr.register_bug_type("Divide by Zero", BugDescription::BI_MEDIUM,
                                               BugDescription::BC_ERROR, "CWE-369");
    m_badShiftTypeId = mgr.register_bug_type("Bad Shift", BugDescription::BI_MEDIUM,
                                              BugDescription::BC_ERROR, "Invalid shift amount");
    m_arrayOOBTypeId = mgr.register_bug_type("Array Out of Bounds", BugDescription::BI_HIGH,
                                              BugDescription::BC_SECURITY, "CWE-119, CWE-125");
    m_deadBranchTypeId = mgr.register_bug_type("Dead Branch", BugDescription::BI_LOW,
                                                BugDescription::BC_ERROR, "Unreachable code");
}

void MKintPass::backedge_analysis(const Function& F) {
    for (const auto& bb_ref : F) {
        const auto *bb = &bb_ref;
        if (m_backedges.count(bb) == 0) {
            // compute backedges of bb
            m_backedges[bb] = {};
            std::vector<const BasicBlock*> remote_succs { bb };
            while (!remote_succs.empty()) {
                const auto *cur_succ = remote_succs.back();
                remote_succs.pop_back();
                for (const auto *const succ : successors(cur_succ)) {
                    if (succ != bb && !m_backedges[bb].contains(succ)) {
                        m_backedges[bb].insert(succ);
                        remote_succs.push_back(succ);
                    }
                }
            }
        }
    }
}

PreservedAnalyses MKintPass::run(Module& M, ModuleAnalysisManager& MAM) {
    MKINT_LOG() << "Running MKint pass on module " << M.getName();
    
    // Apply the CheckAll flag if set to true
    if (CheckAll) {
        CheckIntOverflow = true;
        CheckDivByZero = true;
        CheckBadShift = true;
        CheckArrayOOB = true;
        CheckDeadBranch = true;
    }
    
    // Print checker configuration
    MKINT_LOG() << "Checker Configuration:";
    MKINT_LOG() << "  Integer Overflow: " << (CheckIntOverflow ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Division by Zero: " << (CheckDivByZero ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Bad Shift: " << (CheckBadShift ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Array Out of Bounds: " << (CheckArrayOOB ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Dead Branch: " << (CheckDeadBranch ? "Enabled" : "Disabled");
    
    // Warn if no checkers are enabled
    if (!CheckIntOverflow && !CheckDivByZero && !CheckBadShift && !CheckArrayOOB && !CheckDeadBranch) {
        MKINT_WARN() << "No bug checkers are enabled. No bugs will be detected.";
        MKINT_WARN() << "Use --check-all=true or enable individual checkers with --check-<checker-name>=true";
    }

    // FIXME: This is a hack.
    auto *ctx = new z3::context; // let it leak.
    m_solver = z3::solver(*ctx);
    m_dl = &M.getDataLayout();
    m_ptr_bits = m_dl->getPointerSizeInBits(0);
    m_smt_mem = std::make_unique<SmtMemory>(*ctx, m_ptr_bits);
    m_obj_base.clear();
    m_obj_size.clear();
    m_obj_list.clear();
    m_sym_change_log.clear();
    m_sym_change_frames.clear();

    // Mark taint sources.
    for (auto& F : M) {
        auto taint_sources = m_taint_analysis->get_taint_source(F);
        m_taint_analysis->mark_func_sinks(F, m_callback_tsrc_fn);
        if (TaintAnalysis::is_taint_src(F.getName()))
            m_func2tsrc[&F] = std::move(taint_sources);
    }

    // Propagate taint across functions
    m_taint_analysis->propagate_taint_across_functions(M, m_func2tsrc, m_taint_funcs);

    // Also add main function to analysis if it exists and is not already in taint_funcs
    for (auto& F : M) {
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

    this->init_ranges(M);

    const DataLayout& DL = M.getDataLayout();
    auto& FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    
    constexpr size_t max_try = 128;
    size_t try_count = 0;

    while (true) { // iterative range analysis.
        const auto old_fn_rng = m_func2range_info;
        const auto old_glb_rng = m_global2range;
        const auto old_glb_arrrng = m_garr2ranges;
        const auto old_fn_ret_rng = m_func2ret_range;

        for (auto *F : m_range_analysis_funcs) {
            llvm::AAResults* AA = nullptr;
            llvm::MemorySSA* MSSA = nullptr;
            if (auto *AARes = FAM.getCachedResult<llvm::AAManager>(*F)) {
                AA = AARes;
            } else {
                AA = &FAM.getResult<llvm::AAManager>(*F);
            }
            if (auto *MSSARes = FAM.getCachedResult<llvm::MemorySSAAnalysis>(*F)) {
                MSSA = &MSSARes->getMSSA();
            } else {
                MSSA = &FAM.getResult<llvm::MemorySSAAnalysis>(*F).getMSSA();
            }

            m_range_analysis->range_analysis(*F, m_func2range_info, m_backedges, 
                                           m_global2range, m_garr2ranges, m_func2ret_range,
                                           m_impossible_branches, m_gep_oob, m_func2tsrc, m_callback_tsrc_fn,
                                           DL, AA, MSSA);
        }

        if (m_func2range_info == old_fn_rng && old_glb_rng == m_global2range && old_fn_ret_rng == m_func2ret_range
            && old_glb_arrrng == m_garr2ranges)
            break;
        if (++try_count > max_try) {
            MKINT_LOG() << "[Iterative Range Analysis] "
                        << "Max try " << max_try << " reached, aborting.";
            break;
        }
    }
    
    this->pring_all_ranges();

    this->smt_solving(M);

    m_bug_detection->mark_errors(m_impossible_branches, m_gep_oob, 
                                m_overflow_insts, m_bad_shift_insts, m_div_zero_insts);

    // Report bugs to BugReportMgr (Clearblue pattern)
    reportBugsToManager();

    // Note: SARIF/JSON output is now handled centrally by BugReportMgr
    // in the tool driver, not by individual checkers

    return PreservedAnalyses::all();
}

void MKintPass::init_ranges(Module& M) {
    m_range_analysis->init_ranges(M, m_func2range_info, m_func2ret_range, 
                                 m_range_analysis_funcs, m_global2range, m_garr2ranges,
                                 m_taint_funcs, m_callback_tsrc_fn);
}

void MKintPass::pring_all_ranges() const {
    m_range_analysis->print_all_ranges(m_func2ret_range, m_global2range, m_garr2ranges,
                                      m_func2range_info, m_impossible_branches, m_gep_oob);
}

void MKintPass::smt_solving(Module& M) {
    for (auto *F : m_taint_funcs) {
        if (F->isDeclaration())
            continue;

        // Reset per-function SMT state (kept inside the solver push/pop).
        m_bbpaths.clear();
        m_v2sym.clear();
        m_smt_mem->reset();
        m_obj_base.clear();
        m_obj_size.clear();
        m_obj_list.clear();
        m_sym_change_log.clear();
        m_sym_change_frames.clear();

        // Record start time for this function
        m_function_start_time = std::chrono::steady_clock::now();
        MKINT_LOG() << "Beginning analysis of function " << F->getName();

        // Seed global objects (base address + size) so pointer arithmetic and loads/stores have a model.
        for (auto& GV : M.globals()) {
            const uint64_t bytes = m_dl->getTypeAllocSize(GV.getValueType());
            ensureObject(&GV, ("global." + GV.getName()).str(), m_solver.getValue().ctx().bv_val(bytes, m_ptr_bits),
                         /*sizeKnown=*/true);
        }

        // Seed stack objects (allocas) with constant sizes when possible.
        for (auto& bb : F->getBasicBlockList()) {
            for (auto& inst : bb) {
                if (auto *ai = dyn_cast<AllocaInst>(&inst)) {
                    const uint64_t elemBytes = m_dl->getTypeAllocSize(ai->getAllocatedType());
                    uint64_t totalBytes = elemBytes;
                    bool known = true;
                    if (ai->isArrayAllocation()) {
                        if (auto *ci = dyn_cast<ConstantInt>(ai->getArraySize())) {
                            totalBytes = elemBytes * ci->getZExtValue();
                        } else {
                            known = false;
                        }
                    }
                    ensureObject(ai,
                                 ("alloca." + F->getName().str() + "." + std::to_string((uintptr_t)ai)),
                                 m_solver.getValue().ctx().bv_val(totalBytes, m_ptr_bits), known);
                }
            }
        }

        // Get a path tree.
        for (auto& bb : F->getBasicBlockList()) {
            for (const auto& pred : predecessors(&bb)) {
                if (m_backedges[&bb].contains(pred) || &bb == pred)
                    continue;

                m_bbpaths[pred].push_back(&bb);
            }
        }

        m_solver.getValue().push();
        pushSymFrame();
        m_smt_mem->push();

        // add function arg constraints (integers + pointers).
        for (auto& arg : F->args()) {
            const auto arg_name = F->getName() + "." + std::to_string(arg.getArgNo());
            if (arg.getType()->isIntegerTy()) {
                const auto argv = m_solver.getValue().ctx().bv_const(arg_name.str().c_str(),
                                                                    arg.getType()->getIntegerBitWidth());
                setSym(&arg, argv);
                m_bug_detection->add_range_cons(
                    m_range_analysis->get_range_by_bb(&arg, &(F->getEntryBlock()), m_func2range_info), argv,
                    m_solver.getValue());
            } else if (arg.getType()->isPointerTy()) {
                const auto argv = m_solver.getValue().ctx().bv_const((arg_name + ".ptr").str().c_str(), m_ptr_bits);
                setSym(&arg, argv);
            }
        }

        path_solving(&(F->getEntryBlock()), nullptr);

        m_smt_mem->pop();
        popSymFrame();
        m_solver.getValue().pop();
        
        // Report analysis time
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_function_start_time).count();
        MKINT_LOG() << "Completed analysis of function " << F->getName() 
                   << " in " << elapsed << " seconds";
    }
}

void MKintPass::path_solving(BasicBlock* cur, BasicBlock* pred) {
    // Check for timeout
    if (m_function_timeout > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_function_start_time).count();
        if (elapsed > static_cast<int64_t>(m_function_timeout)) {
            MKINT_WARN() << "Timeout reached for function " << cur->getParent()->getName() 
                         << " after " << elapsed << " seconds. Analysis incomplete.";
            return;
        }
    }
    
    if (m_backedges[cur].contains(pred))
        return;

    // Track this basic block in the current execution path
    std::string bbDesc = "Basic block ";
    if (cur->hasName()) {
        bbDesc += cur->getName().str();
    } else {
        bbDesc += "<unnamed>";
    }
    if (cur->getParent()) {
        bbDesc += " in function " + cur->getParent()->getName().str();
    }
    
    PathPoint pathPoint(cur, nullptr, bbDesc);
    m_bug_detection->addPathPoint(pathPoint);

    if (nullptr != pred) {
        auto *terminator = pred->getTerminator();
        auto *br = dyn_cast<BranchInst>(terminator);
        if (br) {
            if (br->isConditional()) {
                const bool is_true_br = br->getSuccessor(0) == cur;
                Value* cond = br->getCondition();

                // If the condition is an ICmp, encode it precisely (including pointer equality).
                z3::expr condBool = m_solver.getValue().ctx().bool_val(true);
                if (auto *cmp = dyn_cast<ICmpInst>(cond)) {
                    // Skip impossible branch check if checker is enabled and range analysis already proved this edge dead.
                    if (CheckDeadBranch && m_impossible_branches.count(cmp) && m_impossible_branches[cmp] == is_true_br) {
                        return;
                    }

                    auto *lhs = cmp->getOperand(0);
                    auto *rhs = cmp->getOperand(1);

                    if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
                        const auto l = getIntExpr(lhs, pred, nullptr);
                        const auto r = getIntExpr(rhs, pred, nullptr);
                        switch (cmp->getPredicate()) {
                        case ICmpInst::ICMP_EQ: condBool = (l == r); break;
                        case ICmpInst::ICMP_NE: condBool = (l != r); break;
                        case ICmpInst::ICMP_SGT: condBool = z3::sgt(l, r); break;
                        case ICmpInst::ICMP_SGE: condBool = z3::sge(l, r); break;
                        case ICmpInst::ICMP_SLT: condBool = z3::slt(l, r); break;
                        case ICmpInst::ICMP_SLE: condBool = z3::sle(l, r); break;
                        case ICmpInst::ICMP_UGT: condBool = z3::ugt(l, r); break;
                        case ICmpInst::ICMP_UGE: condBool = z3::uge(l, r); break;
                        case ICmpInst::ICMP_ULT: condBool = z3::ult(l, r); break;
                        case ICmpInst::ICMP_ULE: condBool = z3::ule(l, r); break;
                        default:
                            MKINT_WARN() << "Unsupported icmp predicate in branch condition: " << *cmp;
                            condBool = m_solver.getValue().ctx().bool_val(true);
                            break;
                        }
                    } else if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
                        const auto l = getPtrExpr(lhs, pred, nullptr);
                        const auto r = getPtrExpr(rhs, pred, nullptr);
                        switch (cmp->getPredicate()) {
                        case ICmpInst::ICMP_EQ: condBool = (l == r); break;
                        case ICmpInst::ICMP_NE: condBool = (l != r); break;
                        default:
                            MKINT_WARN() << "Unsupported pointer icmp predicate in branch condition: " << *cmp;
                            condBool = m_solver.getValue().ctx().bool_val(true);
                            break;
                        }
                    } else {
                        MKINT_WARN() << "Unsupported icmp operand types in branch condition: " << *cmp;
                    }

                    // Record the branch decision in the bug path.
                    std::string branchDesc = std::string("Taking ") + (is_true_br ? "true" : "false") + " branch from condition: ";
                    llvm::raw_string_ostream brOS(branchDesc);
                    brOS << *cmp;
                    PathPoint branchPoint(pred, cmp, brOS.str());
                    m_bug_detection->addPathPoint(branchPoint);
                } else if (cond->getType()->isIntegerTy(1)) {
                    // Generic i1 condition.
                    const auto c = getIntExpr(cond, pred, nullptr);
                    condBool = (c == m_solver.getValue().ctx().bv_val(1, 1));
                } else {
                    MKINT_WARN() << "Unsupported branch condition: " << *cond;
                }

                m_solver.getValue().add(is_true_br ? condBool : !condBool);
                if (m_solver.getValue().check() == z3::unsat) {
                    MKINT_DEBUG() << "[SMT Solving] Pruned unsat edge into " << cur->getName();
                    return;
                }
            }
        } else if (auto *swt = dyn_cast<SwitchInst>(terminator)) {
            auto *cond = swt->getCondition();
            if (cond->getType()->isIntegerTy()) {
                if (swt->getDefaultDest() == cur) { // default
                    // not (all)
                    for (auto c : swt->cases()) {
                        auto *case_val = c.getCaseValue();
                        m_solver.getValue().add(getIntExpr(cond, pred, nullptr)
                            != m_solver.getValue().ctx().bv_val(case_val->getZExtValue(),
                                                                cond->getType()->getIntegerBitWidth()));
                    }
                } else {
                    for (auto c : swt->cases()) {
                        if (c.getCaseSuccessor() == cur) {
                            auto *case_val = c.getCaseValue();
                            m_solver.getValue().add(getIntExpr(cond, pred, nullptr)
                                == m_solver.getValue().ctx().bv_val(case_val->getZExtValue(),
                                                                    cond->getType()->getIntegerBitWidth()));
                            break;
                        }
                    }
                }
            }
        } else {
            // try catch... (thank god, C does not have try-catch)
            // indirectbr... ?
            MKINT_CHECK_ABORT(false) << "Unknown terminator: " << *pred->getTerminator();
        }
    }

    // Resolve PHI nodes in the current block based on the predecessor edge.
    if (pred) {
        for (auto& inst : cur->getInstList()) {
            auto *phi = dyn_cast<PHINode>(&inst);
            if (!phi) break;
            if (Value* incoming = phi->getIncomingValueForBlock(pred)) {
                const auto incomingExpr = getValueExpr(incoming, pred, nullptr);
                setSym(phi, incomingExpr);
                if (phi->getType()->isIntegerTy()) {
                    m_bug_detection->add_range_cons(
                        m_range_analysis->get_range_by_bb(phi, cur, m_func2range_info), incomingExpr,
                        m_solver.getValue());
                }
            }
        }
    }

    for (auto& inst : cur->getInstList()) {
        if (isa<PHINode>(&inst)) continue;

        if (auto *ai = dyn_cast<AllocaInst>(&inst)) {
            // Bind the alloca instruction to its base address.
            if (m_obj_base.count(ai)) {
                setSym(ai, m_obj_base[ai].getValue());
            } else {
                const uint64_t elemBytes = m_dl->getTypeAllocSize(ai->getAllocatedType());
                uint64_t totalBytes = elemBytes;
                bool known = true;
                if (ai->isArrayAllocation()) {
                    if (auto *ci = dyn_cast<ConstantInt>(ai->getArraySize())) {
                        totalBytes = elemBytes * ci->getZExtValue();
                    } else {
                        known = false;
                    }
                }
                ensureObject(ai, ("alloca." + cur->getParent()->getName().str() + "." + std::to_string((uintptr_t)ai)),
                             m_solver.getValue().ctx().bv_val(totalBytes, m_ptr_bits), known);
                setSym(ai, m_obj_base[ai].getValue());
            }
            continue;
        }

        if (auto *gep = dyn_cast<GetElementPtrInst>(&inst)) {
            setSym(gep, getPtrExpr(gep, cur, pred));
            continue;
        }

        if (auto *load = dyn_cast<LoadInst>(&inst)) {
            if (load->getType()->isIntegerTy()) {
                const auto addr = getPtrExpr(load->getPointerOperand(), cur, pred);
                const unsigned bw = load->getType()->getIntegerBitWidth();
                const unsigned bytes = static_cast<unsigned>(m_dl->getTypeStoreSize(load->getType()));
                const auto v = m_smt_mem->loadInt(addr, bw, bytes, isLittleEndian());
                setSym(load, v);
                if (!m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(load, cur, m_func2range_info), v,
                                                     m_solver.getValue()))
                    return;
            }
            continue;
        }

        if (auto *store = dyn_cast<StoreInst>(&inst)) {
            auto *val = store->getValueOperand();
            if (val && val->getType()->isIntegerTy()) {
                const auto addr = getPtrExpr(store->getPointerOperand(), cur, pred);
                const unsigned bw = val->getType()->getIntegerBitWidth();
                const unsigned bytes = static_cast<unsigned>(m_dl->getTypeStoreSize(val->getType()));
                const auto v = getIntExpr(val, cur, pred);
                m_smt_mem->storeInt(addr, v, bw, bytes, isLittleEndian());
            }
            continue;
        }

        if (auto *call = dyn_cast<CallInst>(&inst)) {
            // Model common allocators as fresh, disjoint heap objects.
            if (call->getType()->isPointerTy()) {
                Function* callee = call->getCalledFunction();
                if (callee) {
                    const auto name = callee->getName();
                    z3::expr sizeBytes = m_solver.getValue().ctx().bv_val(0, m_ptr_bits);
                    bool sizeKnown = false;
                    if (name == "malloc" || name == "kmalloc" || name == "kzalloc" || name == "vmalloc") {
                        if (call->arg_size() >= 1 && call->getArgOperand(0)->getType()->isIntegerTy()) {
                            sizeBytes = getIntExpr(call->getArgOperand(0), cur, pred);
                            const unsigned abw = sizeBytes.get_sort().bv_size();
                            if (abw < m_ptr_bits) sizeBytes = z3::zext(sizeBytes, m_ptr_bits - abw);
                            if (abw > m_ptr_bits) sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
                        }
                        sizeKnown = false;
                    } else if (name == "calloc") {
                        if (call->arg_size() >= 2 && call->getArgOperand(0)->getType()->isIntegerTy()
                            && call->getArgOperand(1)->getType()->isIntegerTy()) {
                            auto n = getIntExpr(call->getArgOperand(0), cur, pred);
                            auto m = getIntExpr(call->getArgOperand(1), cur, pred);
                            const unsigned n_bw = n.get_sort().bv_size();
                            const unsigned m_bw = m.get_sort().bv_size();
                            const unsigned target = std::max(std::max(n_bw, m_bw), m_ptr_bits);
                            if (n_bw < target) n = z3::zext(n, target - n_bw);
                            if (m_bw < target) m = z3::zext(m, target - m_bw);
                            sizeBytes = (n * m);
                            if (target > m_ptr_bits) sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
                        }
                        sizeKnown = false;
                    }

                    if (name == "malloc" || name == "kmalloc" || name == "kzalloc" || name == "vmalloc"
                        || name == "calloc") {
                        ensureObject(call, ("heap." + cur->getParent()->getName().str() + "." + std::to_string((uintptr_t)call)),
                                     sizeBytes, sizeKnown);
                        setSym(call, m_obj_base[call].getValue());
                        continue;
                    }
                }

                // Unknown pointer-returning call: treat as fresh pointer value.
                setSym(call, getPtrExpr(call, cur, pred));
                continue;
            }
        }

        // Integer SSA: keep existing bug checks, but also allow values derived from loads, selects, etc.
        if (inst.getType()->isIntegerTy()) {
            if (auto *op = dyn_cast<BinaryOperator>(&inst)) {
                (void)getIntExpr(op->getOperand(0), cur, pred);
                (void)getIntExpr(op->getOperand(1), cur, pred);
                m_bug_detection->binary_check(op, m_solver.getValue(), m_v2sym, m_overflow_insts, m_bad_shift_insts,
                                              m_div_zero_insts);
                const auto r = m_bug_detection->binary_op_propagate(op, m_v2sym, m_solver.getValue());
                setSym(op, r);
                if (!m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(op, cur, m_func2range_info), r,
                                                     m_solver.getValue()))
                    return;
            } else if (auto *op = dyn_cast<CastInst>(&inst)) {
                (void)getValueExpr(op->getOperand(0), cur, pred);
                const auto r = m_bug_detection->cast_op_propagate(op, m_v2sym, m_solver.getValue());
                setSym(op, r);
                if (!m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(op, cur, m_func2range_info), r,
                                                     m_solver.getValue()))
                    return;
            } else {
                (void)getIntExpr(&inst, cur, pred);
            }
        } else if (inst.getType()->isPointerTy()) {
            (void)getPtrExpr(&inst, cur, pred);
        }
    }

    for (auto *succ : m_bbpaths[cur]) {
        m_solver.getValue().push();
        pushSymFrame();
        m_smt_mem->push();
        path_solving(succ, cur);
        m_smt_mem->pop();
        popSymFrame();
        m_solver.getValue().pop();
    }
    
    // Pop the current basic block from the path when backtracking
    auto currentPath = m_bug_detection->getCurrentPath();
    if (!currentPath.empty()) {
        currentPath.pop_back();
        m_bug_detection->setCurrentPath(currentPath);
    }
}


std::string MKintPass::get_bb_label(const BasicBlock* bb) {
    // Check if func and module are available, avoiding 'Segmentfault'
    if (!bb || !bb->getParent() || bb->getParent()->getName().empty() || !bb->getParent()->getParent()) return "<badref>";
    std::string str;
    llvm::raw_string_ostream os(str);
    bb->printAsOperand(os, false);
    return str;
}

void MKintPass::generateSarifReport(const std::string& filename) {
    if (m_bug_detection) {
        m_bug_detection->generateSarifReport(filename, m_impossible_branches, m_gep_oob,
                                            m_overflow_insts, m_bad_shift_insts, m_div_zero_insts);
    }
}

void MKintPass::pushSymFrame() {
    m_sym_change_frames.push_back(m_sym_change_log.size());
}

void MKintPass::popSymFrame() {
    if (m_sym_change_frames.empty()) return;
    const size_t frameStart = m_sym_change_frames.back();
    m_sym_change_frames.pop_back();

    while (m_sym_change_log.size() > frameStart) {
        const SymChange ch = m_sym_change_log.back();
        m_sym_change_log.pop_back();
        if (!ch.key) continue;
        if (ch.hadOld) {
            m_v2sym[ch.key] = ch.oldValue;
        } else {
            m_v2sym.erase(ch.key);
        }
    }
}

void MKintPass::setSym(const Value* v, const z3::expr& e) {
    SymChange ch;
    ch.key = v;
    auto it = m_v2sym.find(v);
    ch.hadOld = (it != m_v2sym.end());
    if (ch.hadOld) ch.oldValue = it->second;
    m_sym_change_log.push_back(ch);
    m_v2sym[v] = e;
}

bool MKintPass::isLittleEndian() const {
    return m_dl ? m_dl->isLittleEndian() : true;
}

void MKintPass::ensureObject(const Value* obj, const std::string& hintName, const z3::expr& sizeBytes,
                             bool sizeKnown) {
    if (m_obj_base.count(obj)) return;

    auto& ctx = m_solver.getValue().ctx();
    const auto base = ctx.bv_const(hintName.c_str(), m_ptr_bits);

    m_obj_base[obj] = base;
    m_obj_size[obj] = sizeBytes;
    m_obj_list.push_back(obj);

    // Basic well-formedness: keep base non-zero to avoid conflating with null.
    m_solver.getValue().add(base != ctx.bv_val(0, m_ptr_bits));

    // Disjointness constraints against previously created objects.
    for (const auto* other : m_obj_list) {
        if (other == obj) continue;
        if (!m_obj_base.count(other) || !m_obj_size.count(other)) continue;
        const auto otherBase = m_obj_base[other].getValue();
        const auto otherSize = m_obj_size[other].getValue();
        if (sizeKnown) {
            // Non-overlap: [base, base+size) does not overlap [otherBase, otherBase+otherSize)
            const auto endThis = base + sizeBytes;
            const auto endOther = otherBase + otherSize;
            m_solver.getValue().add(z3::ule(endThis, otherBase) || z3::ule(endOther, base));
        } else {
            // Unknown size: at least force distinct bases.
            m_solver.getValue().add(base != otherBase);
        }
    }
}

z3::expr MKintPass::getValueExpr(const Value* v, BasicBlock* cur, BasicBlock* pred) {
    if (!v) return m_solver.getValue().ctx().bv_val(0, 1);
    if (v->getType()->isIntegerTy()) return getIntExpr(v, cur, pred);
    if (v->getType()->isPointerTy()) return getPtrExpr(v, cur, pred);
    // Unsupported sort: return a fresh 1-bit value to keep the solver going.
    const std::string name = "%unsupported." + std::to_string((uintptr_t)v);
    return m_solver.getValue().ctx().bv_const(name.c_str(), 1);
}

z3::expr MKintPass::getIntExpr(const Value* v, BasicBlock* cur, BasicBlock* pred) {
    auto it = m_v2sym.find(v);
    if (it != m_v2sym.end()) return it->second.getValue();

    auto& ctx = m_solver.getValue().ctx();

    if (const auto* ci = dyn_cast<ConstantInt>(v)) {
        return ctx.bv_val(ci->getZExtValue(), ci->getType()->getIntegerBitWidth());
    }

    if (const auto* pti = dyn_cast<PtrToIntInst>(v)) {
        auto p = getPtrExpr(pti->getOperand(0), cur, pred);
        const unsigned bw = pti->getType()->getIntegerBitWidth();
        if (bw < m_ptr_bits) p = p.extract(bw - 1, 0);
        else if (bw > m_ptr_bits) p = z3::zext(p, bw - m_ptr_bits);
        setSym(v, p);
        return p;
    }

    if (const auto* itp = dyn_cast<IntToPtrInst>(v)) {
        auto i = getIntExpr(itp->getOperand(0), cur, pred);
        const unsigned ibw = i.get_sort().bv_size();
        if (ibw < m_ptr_bits) i = z3::zext(i, m_ptr_bits - ibw);
        else if (ibw > m_ptr_bits) i = i.extract(m_ptr_bits - 1, 0);
        // IntToPtr result is a pointer, not int; fall back to fresh int symbol.
    }

    if (const auto* sel = dyn_cast<SelectInst>(v)) {
        if (sel->getType()->isIntegerTy()) {
            auto c = getIntExpr(sel->getCondition(), cur, pred);
            auto t = getIntExpr(sel->getTrueValue(), cur, pred);
            auto f = getIntExpr(sel->getFalseValue(), cur, pred);
            z3::expr condBool = (c == ctx.bv_val(1, 1));
            z3::expr r = z3::ite(condBool, t, f);
            setSym(v, r);
            return r;
        }
    }

    if (const auto* icmp = dyn_cast<ICmpInst>(v)) {
        auto *lhs = icmp->getOperand(0);
        auto *rhs = icmp->getOperand(1);
        z3::expr condBool = ctx.bool_val(true);
        if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
            const auto l = getIntExpr(lhs, cur, pred);
            const auto r = getIntExpr(rhs, cur, pred);
            switch (icmp->getPredicate()) {
            case ICmpInst::ICMP_EQ: condBool = (l == r); break;
            case ICmpInst::ICMP_NE: condBool = (l != r); break;
            case ICmpInst::ICMP_SGT: condBool = z3::sgt(l, r); break;
            case ICmpInst::ICMP_SGE: condBool = z3::sge(l, r); break;
            case ICmpInst::ICMP_SLT: condBool = z3::slt(l, r); break;
            case ICmpInst::ICMP_SLE: condBool = z3::sle(l, r); break;
            case ICmpInst::ICMP_UGT: condBool = z3::ugt(l, r); break;
            case ICmpInst::ICMP_UGE: condBool = z3::uge(l, r); break;
            case ICmpInst::ICMP_ULT: condBool = z3::ult(l, r); break;
            case ICmpInst::ICMP_ULE: condBool = z3::ule(l, r); break;
            default: break;
            }
        } else if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
            const auto l = getPtrExpr(lhs, cur, pred);
            const auto r = getPtrExpr(rhs, cur, pred);
            switch (icmp->getPredicate()) {
            case ICmpInst::ICMP_EQ: condBool = (l == r); break;
            case ICmpInst::ICMP_NE: condBool = (l != r); break;
            default: break;
            }
        }
        auto bv = z3::ite(condBool, ctx.bv_val(1, 1), ctx.bv_val(0, 1));
        setSym(v, bv);
        return bv;
    }

    if (const auto* phi = dyn_cast<PHINode>(v)) {
        // Ideally resolved on block entry. If not, keep it symbolic.
        const std::string name = "%phi." + std::to_string((uintptr_t)phi);
        auto r = ctx.bv_const(name.c_str(), phi->getType()->getIntegerBitWidth());
        setSym(v, r);
        return r;
    }

    if (const auto* call = dyn_cast<CallInst>(v)) {
        if (call->getType()->isIntegerTy()) {
            const std::string name = "%call." + std::to_string((uintptr_t)call);
            auto r = ctx.bv_const(name.c_str(), call->getType()->getIntegerBitWidth());
            setSym(v, r);
            if (cur) {
                m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(call, cur, m_func2range_info), r,
                                                m_solver.getValue());
            }
            return r;
        }
    }

    // Default: fresh int with range constraints if available.
    const unsigned bw = v->getType()->getIntegerBitWidth();
    const std::string name = "%int." + std::to_string((uintptr_t)v);
    auto r = ctx.bv_const(name.c_str(), bw);
    setSym(v, r);
    if (cur) {
        m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(v, cur, m_func2range_info), r,
                                        m_solver.getValue());
    }
    return r;
}

z3::expr MKintPass::gepOffsetBytes(const GetElementPtrInst* gep, BasicBlock* cur, BasicBlock* pred) {
    auto& ctx = m_solver.getValue().ctx();
    if (!gep || !m_dl) return ctx.bv_val(0, m_ptr_bits);

    // Fast path: all-constant GEP.
    APInt constOff(m_ptr_bits, 0);
    if (gep->accumulateConstantOffset(*m_dl, constOff)) {
        return ctx.bv_val(constOff.getZExtValue(), m_ptr_bits);
    }

    z3::expr off = ctx.bv_val(0, m_ptr_bits);
    Type* ty = gep->getSourceElementType();
    unsigned idxNo = 0;
    for (const auto *idxIt = gep->idx_begin(); idxIt != gep->idx_end(); ++idxIt, ++idxNo) {
        Value* idxV = idxIt->get();
        if (!idxV) continue;

        if (auto *st = dyn_cast<StructType>(ty)) {
            auto *ci = dyn_cast<ConstantInt>(idxV);
            if (!ci) {
                // Non-constant struct indices are not supported in LLVM IR, but be defensive.
                const std::string name = "%gep.structidx." + std::to_string((uintptr_t)gep);
                return ctx.bv_const(name.c_str(), m_ptr_bits);
            }
            const unsigned field = static_cast<unsigned>(ci->getZExtValue());
            const auto* layout = m_dl->getStructLayout(st);
            off = off + ctx.bv_val(layout->getElementOffset(field), m_ptr_bits);
            ty = st->getElementType(field);
            continue;
        }

        uint64_t elemBytes = 0;
        if (auto *at = dyn_cast<ArrayType>(ty)) {
            elemBytes = m_dl->getTypeAllocSize(at->getElementType());
            ty = at->getElementType();
        } else {
            // First index on a scalar pointer: step by the source element size.
            elemBytes = m_dl->getTypeAllocSize(ty);
        }

        z3::expr idx = getIntExpr(idxV, cur, pred);
        const unsigned ibw = idx.get_sort().bv_size();
        if (ibw < m_ptr_bits) idx = z3::sext(idx, m_ptr_bits - ibw);
        else if (ibw > m_ptr_bits) idx = idx.extract(m_ptr_bits - 1, 0);
        off = off + (idx * ctx.bv_val(elemBytes, m_ptr_bits));
    }

    return off;
}

z3::expr MKintPass::getPtrExpr(const Value* v, BasicBlock* cur, BasicBlock* pred) {
    auto it = m_v2sym.find(v);
    if (it != m_v2sym.end()) return it->second.getValue();

    auto& ctx = m_solver.getValue().ctx();

    if (isa<ConstantPointerNull>(v)) {
        return ctx.bv_val(0, m_ptr_bits);
    }

    if (const auto* gv = dyn_cast<GlobalVariable>(v)) {
        if (!m_obj_base.count(gv)) {
            const uint64_t bytes = m_dl->getTypeAllocSize(gv->getValueType());
            ensureObject(gv, ("global." + gv->getName()).str(), ctx.bv_val(bytes, m_ptr_bits), true);
        }
        setSym(v, m_obj_base[gv].getValue());
        return m_obj_base[gv].getValue();
    }

    if (const auto* ai = dyn_cast<AllocaInst>(v)) {
        if (!m_obj_base.count(ai)) {
            const uint64_t elemBytes = m_dl->getTypeAllocSize(ai->getAllocatedType());
            uint64_t totalBytes = elemBytes;
            bool known = true;
            if (ai->isArrayAllocation()) {
                if (auto *ci = dyn_cast<ConstantInt>(ai->getArraySize())) {
                    totalBytes = elemBytes * ci->getZExtValue();
                } else {
                    known = false;
                }
            }
            ensureObject(ai,
                         ("alloca." + ai->getFunction()->getName().str() + "." + std::to_string((uintptr_t)ai)),
                         ctx.bv_val(totalBytes, m_ptr_bits), known);
        }
        setSym(v, m_obj_base[ai].getValue());
        return m_obj_base[ai].getValue();
    }

    if (const auto* arg = dyn_cast<Argument>(v)) {
        if (arg->getType()->isPointerTy()) {
            const std::string name = (arg->getParent()->getName() + ".argptr" + std::to_string(arg->getArgNo())).str();
            auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
            setSym(v, r);
            return r;
        }
    }

    if (const auto* gep = dyn_cast<GetElementPtrInst>(v)) {
        auto base = getPtrExpr(gep->getPointerOperand(), cur, pred);
        auto off = gepOffsetBytes(gep, cur, pred);
        auto r = base + off;
        setSym(v, r);
        return r;
    }

    if (const auto* bc = dyn_cast<BitCastInst>(v)) {
        auto r = getPtrExpr(bc->getOperand(0), cur, pred);
        setSym(v, r);
        return r;
    }

    if (const auto* itp = dyn_cast<IntToPtrInst>(v)) {
        auto i = getIntExpr(itp->getOperand(0), cur, pred);
        const unsigned ibw = i.get_sort().bv_size();
        if (ibw < m_ptr_bits) i = z3::zext(i, m_ptr_bits - ibw);
        else if (ibw > m_ptr_bits) i = i.extract(m_ptr_bits - 1, 0);
        setSym(v, i);
        return i;
    }

    if (const auto* sel = dyn_cast<SelectInst>(v)) {
        if (sel->getType()->isPointerTy()) {
            auto c = getIntExpr(sel->getCondition(), cur, pred);
            auto t = getPtrExpr(sel->getTrueValue(), cur, pred);
            auto f = getPtrExpr(sel->getFalseValue(), cur, pred);
            z3::expr condBool = (c == ctx.bv_val(1, 1));
            auto r = z3::ite(condBool, t, f);
            setSym(v, r);
            return r;
        }
    }

    if (const auto* phi = dyn_cast<PHINode>(v)) {
        const std::string name = "%phi.ptr." + std::to_string((uintptr_t)phi);
        auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
        setSym(v, r);
        return r;
    }

    if (const auto* call = dyn_cast<CallInst>(v)) {
        if (call->getType()->isPointerTy()) {
            const std::string name = "%call.ptr." + std::to_string((uintptr_t)call);
            auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
            setSym(v, r);
            return r;
        }
    }

    const std::string name = "%ptr." + std::to_string((uintptr_t)v);
    auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
    setSym(v, r);
    return r;
}

} // namespace kint
