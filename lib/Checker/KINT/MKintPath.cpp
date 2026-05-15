#include "Checker/KINT/MKintAnalysisSupport.h"

#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

using namespace llvm;

namespace kint {

void MKintPass::path_solving(BasicBlock *cur, BasicBlock *pred) {
  if (m_summary_failed)
    return;
  // Backedge check must come before the path counter so that loop back-edges
  // do not consume path budget.
  if (m_backedges[cur].contains(pred)) {
    if (m_building_summary)
      m_summary_backedge_hit = true;
    return;
  }

  // Cap path exploration to avoid blowups on large CFGs.
  if (m_path_limit > 0) {
    if (m_paths_explored++ >= m_path_limit) {
      if (!m_path_limit_hit) {
        MKINT_WARN() << "Path exploration limit reached for function "
                     << (cur && cur->getParent() ? cur->getParent()->getName()
                                                 : "<unknown>")
                     << " (limit=" << m_path_limit << "). Analysis incomplete.";
        m_path_limit_hit = true;
      }
      return;
    }
  }

  // Check for timeout
  if (m_function_timeout > 0) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - m_function_start_time)
                       .count();
    if (elapsed > static_cast<int64_t>(m_function_timeout)) {
      MKINT_WARN() << "Timeout reached for function "
                   << cur->getParent()->getName() << " after " << elapsed
                   << " seconds. Analysis incomplete.";
      m_timeout_hit = true;
      return;
    }
  }

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

  auto pointerAccessBytes = [&](const Value *ptr) -> uint64_t {
    if (!ptr || !ptr->getType()->isPointerTy())
      return 0;
    Type *elemTy = ptr->getType()->getPointerElementType();
    if (!elemTy || elemTy->isVoidTy() || elemTy->isFunctionTy())
      return 0;
    return m_dl->getTypeStoreSize(elemTy);
  };

  auto havocObjectAtPtr = [&](const Value *obj, const Value *ptr,
                              uint64_t numBytes,
                              const std::string &hint) -> bool {
    if (!obj || !ptr || numBytes == 0)
      return false;
    if (!m_obj_base.count(obj) || !m_obj_base[obj].has_value())
      return false;
    const auto addr = getPtrExpr(ptr, cur, pred);
    const auto base = m_obj_base[obj].value();
    havocObjectRange(obj, addr - base, numBytes, hint);
    return true;
  };

  if (nullptr != pred) {
    auto *terminator = pred->getTerminator();
    auto *br = dyn_cast<BranchInst>(terminator);
    if (br) {
      if (br->isConditional()) {
        const bool is_true_br = br->getSuccessor(0) == cur;
        Value *cond = br->getCondition();

        // If the condition is an ICmp, encode it precisely (including pointer
        // equality).
        z3::expr condBool = m_solver.value().ctx().bool_val(true);
        if (auto *cmp = dyn_cast<ICmpInst>(cond)) {
          // Do not hard-prune based on range analysis; let SMT decide
          // satisfiability.

          auto *lhs = cmp->getOperand(0);
          auto *rhs = cmp->getOperand(1);

          if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
            const auto l = getIntExpr(lhs, pred, nullptr);
            const auto r = getIntExpr(rhs, pred, nullptr);
            switch (cmp->getPredicate()) {
            case ICmpInst::ICMP_EQ:
              condBool = (l == r);
              break;
            case ICmpInst::ICMP_NE:
              condBool = (l != r);
              break;
            case ICmpInst::ICMP_SGT:
              condBool = z3::sgt(l, r);
              break;
            case ICmpInst::ICMP_SGE:
              condBool = z3::sge(l, r);
              break;
            case ICmpInst::ICMP_SLT:
              condBool = z3::slt(l, r);
              break;
            case ICmpInst::ICMP_SLE:
              condBool = z3::sle(l, r);
              break;
            case ICmpInst::ICMP_UGT:
              condBool = z3::ugt(l, r);
              break;
            case ICmpInst::ICMP_UGE:
              condBool = z3::uge(l, r);
              break;
            case ICmpInst::ICMP_ULT:
              condBool = z3::ult(l, r);
              break;
            case ICmpInst::ICMP_ULE:
              condBool = z3::ule(l, r);
              break;
            default:
              MKINT_WARN() << "Unsupported icmp predicate in branch condition: "
                           << *cmp;
              condBool = m_solver.value().ctx().bool_val(true);
              break;
            }
          } else if (lhs->getType()->isPointerTy() &&
                     rhs->getType()->isPointerTy()) {
            const auto l = getPtrExpr(lhs, pred, nullptr);
            const auto r = getPtrExpr(rhs, pred, nullptr);
            switch (cmp->getPredicate()) {
            case ICmpInst::ICMP_EQ:
              condBool = (l == r);
              break;
            case ICmpInst::ICMP_NE:
              condBool = (l != r);
              break;
            default:
              MKINT_WARN()
                  << "Unsupported pointer icmp predicate in branch condition: "
                  << *cmp;
              condBool = m_solver.value().ctx().bool_val(true);
              break;
            }
          } else {
            MKINT_WARN()
                << "Unsupported icmp operand types in branch condition: "
                << *cmp;
          }

          // Record the branch decision in the bug path.
          std::string branchDesc = std::string("Taking ") +
                                   (is_true_br ? "true" : "false") +
                                   " branch from condition: ";
          llvm::raw_string_ostream brOS(branchDesc);
          brOS << *cmp;
          PathPoint branchPoint(pred, cmp, brOS.str());
          m_bug_detection->addPathPoint(branchPoint);
        } else if (cond->getType()->isIntegerTy(1)) {
          // Generic i1 condition.
          const auto c = getIntExpr(cond, pred, nullptr);
          condBool = (c == m_solver.value().ctx().bv_val(1, 1));
        } else {
          MKINT_WARN() << "Unsupported branch condition: " << *cond;
        }

        addConstraint(is_true_br ? condBool : !condBool);
        if (m_solver.value().check() == z3::unsat) {
          if (!m_building_summary && CheckDeadBranch) {
            if (auto *cmp = dyn_cast<ICmpInst>(cond))
              m_impossible_branches[cmp] = is_true_br;
          }
          MKINT_DEBUG() << "[SMT Solving] Pruned unsat edge into "
                        << cur->getName();
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
            addConstraint(
                getIntExpr(cond, pred, nullptr) !=
                bvValFromAPInt(m_solver.value().ctx(), case_val->getValue()));
          }
        } else {
          for (auto c : swt->cases()) {
            if (c.getCaseSuccessor() == cur) {
              auto *case_val = c.getCaseValue();
              addConstraint(
                  getIntExpr(cond, pred, nullptr) ==
                  bvValFromAPInt(m_solver.value().ctx(), case_val->getValue()));
              break;
            }
          }
        }
      }
    } else if (isa<InvokeInst>(terminator) || isa<IndirectBrInst>(terminator) ||
               isa<CallBrInst>(terminator)) {
      // No additional constraints; successor feasibility handled
      // conservatively.
    } else {
      // try catch... (thank god, C does not have try-catch)
      // indirectbr... ?
      MKINT_WARN() << "Unknown terminator; proceeding conservatively: "
                   << *pred->getTerminator();
    }
  }

  // Resolve PHI nodes in the current block based on the predecessor edge.
  if (pred) {
    for (auto &inst : cur->getInstList()) {
      auto *phi = dyn_cast<PHINode>(&inst);
      if (!phi)
        break;
      if (Value *incoming = phi->getIncomingValueForBlock(pred)) {
        const auto incomingExpr = getValueExpr(incoming, pred, nullptr);
        setSym(phi, incomingExpr);
      }
    }
  }

  for (auto &inst : cur->getInstList()) {
    if (isa<PHINode>(&inst))
      continue;

    if (auto *ret = dyn_cast<ReturnInst>(&inst)) {
      if (m_building_summary && !recordSummaryCase(ret, cur, pred)) {
        m_summary_failed = true;
        return;
      }
      continue;
    }

    if (auto *assumeI = dyn_cast<AssumeInst>(&inst)) {
      // llvm.assume(cond) adds a constraint to the current path.
      const auto *cond = assumeI->getArgOperand(0);
      if (cond && cond->getType()->isIntegerTy(1)) {
        const auto c = getIntExpr(cond, cur, pred);
        addConstraint(c == m_solver.value().ctx().bv_val(1, 1));
        if (m_solver.value().check() == z3::unsat)
          return;
      }
      continue;
    }

    if (auto *woi = dyn_cast<WithOverflowInst>(&inst)) {
      // Encode arithmetic/overflow semantics for llvm.*with.overflow
      // intrinsics. Bug checking: if overflow is satisfiable under current path
      // constraints, report an overflow.
      if (!m_building_summary && CheckIntOverflow) {
        z3::expr res = m_solver.value().ctx().bv_val(
            0, woi->getArgOperand(0)->getType()->getIntegerBitWidth());
        z3::expr ov = m_solver.value().ctx().bool_val(false);
        if (computeWithOverflow(
                woi, m_solver.value(),
                [&](const llvm::Value *x) { return getIntExpr(x, cur, pred); },
                res, ov)) {
          if (checkBugCondition(woi, interr::INT_OVERFLOW, ov)) {
            m_overflow_insts.insert(woi);
            if (m_bug_detection)
              m_bug_detection->recordBug(woi, interr::INT_OVERFLOW);
          }
        }
      }
      continue;
    }

    // Model memory intrinsics.
    // These often appear as `llvm.memset/memcpy/memmove.*` and bypass normal
    // CallInst handling.
    if (auto *memsetI = dyn_cast<MemSetInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memsetI->getRawDest(), cur, pred);
      const auto val = getIntExpr(memsetI->getValue(), cur, pred);
      if (const auto len = getConstantU64(memsetI->getLength())) {
        if (*len <= kMaxBytes) {
          const Value *obj = getObjectForPtr(memsetI->getRawDest());
          if (obj && m_obj_mem.count(obj)) {
            const auto base = m_obj_base[obj].value();
            const auto off = dst - base;
            z3::expr b = val;
            const unsigned bw = b.get_sort().bv_size();
            if (bw < 8)
              b = z3::zext(b, 8 - bw);
            else if (bw > 8)
              b = b.extract(7, 0);
            z3::expr curMem = m_obj_mem[obj].value();
            for (uint64_t i = 0; i < *len; ++i) {
              curMem = z3::store(
                  curMem, off + m_solver.value().ctx().bv_val(i, m_ptr_bits),
                  b);
            }
            m_obj_mem[obj] = curMem;
          } else {
            m_smt_mem->memsetBytes(dst, val, *len);
          }
        } else {
          const Value *obj = getObjectForPtr(memsetI->getRawDest());
          if (obj)
            havocObject(obj, "memset_large");
          else
            m_smt_mem->havoc("memset_large");
        }
        if (!maybeCheckOOB(memsetI, memsetI->getRawDest(), *len, cur, pred))
          return;
      } else {
        const Value *obj = getObjectForPtr(memsetI->getRawDest());
        if (obj)
          havocObject(obj, "memset_sym");
        else
          m_smt_mem->havoc("memset_sym");
      }
      continue;
    }

    if (auto *memcpyI = dyn_cast<MemCpyInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memcpyI->getRawDest(), cur, pred);
      const auto src = getPtrExpr(memcpyI->getRawSource(), cur, pred);
      if (const auto len = getConstantU64(memcpyI->getLength())) {
        if (*len <= kMaxBytes) {
          const Value *dstObj = getObjectForPtr(memcpyI->getRawDest());
          const Value *srcObj = getObjectForPtr(memcpyI->getRawSource());
          if (dstObj && srcObj && m_obj_mem.count(dstObj) &&
              m_obj_mem.count(srcObj)) {
            const auto dstBase = m_obj_base[dstObj].value();
            const auto srcBase = m_obj_base[srcObj].value();
            z3::expr dstOff = dst - dstBase;
            z3::expr srcOff = src - srcBase;
            z3::expr dstMem = m_obj_mem[dstObj].value();
            z3::expr srcMem = m_obj_mem[srcObj].value();
            for (uint64_t i = 0; i < *len; ++i) {
              z3::expr b = z3::select(
                  srcMem,
                  srcOff + m_solver.value().ctx().bv_val(i, m_ptr_bits));
              dstMem = z3::store(
                  dstMem, dstOff + m_solver.value().ctx().bv_val(i, m_ptr_bits),
                  b);
            }
            m_obj_mem[dstObj] = dstMem;
          } else if (dstObj) {
            havocObject(dstObj, "memcpy_unknown_src");
          } else {
            m_smt_mem->memcpyBytes(dst, src, *len);
          }
        } else {
          const Value *obj = getObjectForPtr(memcpyI->getRawDest());
          if (obj)
            havocObject(obj, "memcpy_large");
          else
            m_smt_mem->havoc("memcpy_large");
        }
        if (!maybeCheckOOB(memcpyI, memcpyI->getRawDest(), *len, cur, pred))
          return;
      } else {
        const Value *obj = getObjectForPtr(memcpyI->getRawDest());
        if (obj)
          havocObject(obj, "memcpy_sym");
        else
          m_smt_mem->havoc("memcpy_sym");
      }
      continue;
    }

    if (auto *memmoveI = dyn_cast<MemMoveInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memmoveI->getRawDest(), cur, pred);
      const auto src = getPtrExpr(memmoveI->getRawSource(), cur, pred);
      if (const auto len = getConstantU64(memmoveI->getLength())) {
        if (*len <= kMaxBytes) {
          // Our memory is a functional array; a forward copy is sufficient for
          // modeling memmove.
          const Value *dstObj = getObjectForPtr(memmoveI->getRawDest());
          const Value *srcObj = getObjectForPtr(memmoveI->getRawSource());
          if (dstObj && srcObj && m_obj_mem.count(dstObj) &&
              m_obj_mem.count(srcObj)) {
            const auto dstBase = m_obj_base[dstObj].value();
            const auto srcBase = m_obj_base[srcObj].value();
            z3::expr dstOff = dst - dstBase;
            z3::expr srcOff = src - srcBase;
            z3::expr dstMem = m_obj_mem[dstObj].value();
            z3::expr srcMem = m_obj_mem[srcObj].value();
            std::vector<z3::expr> snapshot;
            snapshot.reserve(*len);
            for (uint64_t i = 0; i < *len; ++i) {
              snapshot.push_back(z3::select(
                  srcMem, srcOff + m_solver.value().ctx().bv_val(i, m_ptr_bits)));
            }
            for (uint64_t i = 0; i < *len; ++i) {
              dstMem = z3::store(
                  dstMem, dstOff + m_solver.value().ctx().bv_val(i, m_ptr_bits),
                  snapshot[i]);
            }
            m_obj_mem[dstObj] = dstMem;
          } else if (dstObj) {
            havocObject(dstObj, "memmove_unknown_src");
          } else {
            m_smt_mem->memmoveBytes(dst, src, *len);
          }
        } else {
          const Value *obj = getObjectForPtr(memmoveI->getRawDest());
          if (obj)
            havocObject(obj, "memmove_large");
          else
            m_smt_mem->havoc("memmove_large");
        }
        if (!maybeCheckOOB(memmoveI, memmoveI->getRawDest(), *len, cur, pred))
          return;
      } else {
        const Value *obj = getObjectForPtr(memmoveI->getRawDest());
        if (obj)
          havocObject(obj, "memmove_sym");
        else
          m_smt_mem->havoc("memmove_sym");
      }
      continue;
    }

    if (auto *ai = dyn_cast<AllocaInst>(&inst)) {
      // Bind the alloca instruction to its base address.
      if (m_obj_base.count(ai)) {
        setSym(ai, m_obj_base[ai].value());
      } else {
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
            auto countExpr = getIntExpr(ai->getArraySize(), cur, pred);
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
                     ("alloca." + cur->getParent()->getName().str() + "." +
                      std::to_string((uintptr_t)ai)),
                     sizeBytesExpr, known);
        setSym(ai, m_obj_base[ai].value());
      }
      continue;
    }

    if (auto *gep = dyn_cast<GetElementPtrInst>(&inst)) {
      setSym(gep, getPtrExpr(gep, cur, pred));
      continue;
    }

    if (auto *load = dyn_cast<LoadInst>(&inst)) {
      const unsigned bytes =
          static_cast<unsigned>(m_dl->getTypeStoreSize(load->getType()));
      if (!maybeCheckOOB(load, load->getPointerOperand(), bytes, cur, pred))
        return;
      if (load->getType()->isIntegerTy() || load->getType()->isPointerTy()) {
        const auto addr = getPtrExpr(load->getPointerOperand(), cur, pred);
        const unsigned bw = load->getType()->isPointerTy()
                                ? m_ptr_bits
                                : load->getType()->getIntegerBitWidth();
        const Value *obj = getObjectForPtr(load->getPointerOperand());
        const bool hasPreciseObjectMem =
            (obj && m_obj_mem.count(obj) && m_obj_mem[obj].has_value());
        z3::expr v = load->getType()->isPointerTy()
                         ? m_smt_mem->loadBytes(addr, bytes, isLittleEndian())
                         : m_smt_mem->loadInt(addr, bw, bytes, isLittleEndian());
        if (hasPreciseObjectMem) {
          const auto base = m_obj_base[obj].value();
          const auto off = addr - base;
          z3::expr bytesExpr = loadBytesFromMem(m_obj_mem[obj].value(), off,
                                                bytes, isLittleEndian());
          const unsigned loadedBits = bytes * 8;
          if (bw == loadedBits) {
            v = bytesExpr;
          } else if (bw < loadedBits) {
            v = bytesExpr.extract(bw - 1, 0);
          } else {
            v = z3::zext(bytesExpr, bw - loadedBits);
          }
        }
        setSym(load, v);
        if (m_robust_universal_unknown_loads) {
          bool unknown = true;
          const Value *underlying =
              llvm::getUnderlyingObject(load->getPointerOperand());
          if (underlying && m_obj_base.count(underlying)) {
            unknown = false;
          }
          if (unknown) {
            registerUniversal(v);
          }
        }
        if (m_robust_universal_external_globals) {
          if (const auto *gv = dyn_cast_or_null<GlobalVariable>(
                  llvm::getUnderlyingObject(load->getPointerOperand()))) {
            if (gv->isDeclaration()) {
              registerUniversal(v);
            }
          }
        }
      }
      continue;
    }

    if (auto *store = dyn_cast<StoreInst>(&inst)) {
      auto *val = store->getValueOperand();
      const unsigned bytes =
          static_cast<unsigned>(m_dl->getTypeStoreSize(val->getType()));
      if (!maybeCheckOOB(store, store->getPointerOperand(), bytes, cur, pred))
        return;
      if (val && (val->getType()->isIntegerTy() || val->getType()->isPointerTy())) {
        const auto addr = getPtrExpr(store->getPointerOperand(), cur, pred);
        const unsigned bw = val->getType()->isPointerTy()
                                ? m_ptr_bits
                                : val->getType()->getIntegerBitWidth();
        const auto v = val->getType()->isPointerTy()
                            ? getPtrExpr(val, cur, pred)
                            : getIntExpr(val, cur, pred);
        const Value *obj = getObjectForPtr(store->getPointerOperand());
        if (obj && m_obj_mem.count(obj)) {
          const auto base = m_obj_base[obj].value();
          const auto off = addr - base;
          z3::expr newMem = storeBytesToMem(m_obj_mem[obj].value(), off, v,
                                            bytes, isLittleEndian());
          m_obj_mem[obj] = newMem;
        } else {
          m_smt_mem->storeInt(addr, v, bw, bytes, isLittleEndian());
        }
      }
      continue;
    }

    if (auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
      const Value *obj = getObjectForPtr(rmw->getPointerOperand());
      const uint64_t bytes =
          static_cast<uint64_t>(m_dl->getTypeStoreSize(rmw->getValOperand()->getType()));
      if (obj && bytes > 0 &&
          havocObjectAtPtr(obj, rmw->getPointerOperand(), bytes, "atomicrmw")) {
        // handled precisely
      } else if (obj) {
        havocObject(obj, "atomicrmw");
      } else {
        m_smt_mem->havoc("atomicrmw");
      }
      if (rmw->getType()->isIntegerTy()) {
        (void)getIntExpr(rmw, cur, pred);
      }
      continue;
    }

    if (auto *cx = dyn_cast<AtomicCmpXchgInst>(&inst)) {
      const Value *obj = getObjectForPtr(cx->getPointerOperand());
      const uint64_t bytes =
          static_cast<uint64_t>(m_dl->getTypeStoreSize(cx->getCompareOperand()->getType()));
      if (obj && bytes > 0 &&
          havocObjectAtPtr(obj, cx->getPointerOperand(), bytes, "cmpxchg")) {
        // handled precisely
      } else if (obj) {
        havocObject(obj, "cmpxchg");
      } else {
        m_smt_mem->havoc("cmpxchg");
      }
      continue;
    }

    if (auto *call = dyn_cast<CallInst>(&inst)) {
      // Model common libc memory routines (when they survive as regular calls).
      if (Function *callee = call->getCalledFunction()) {
        constexpr uint64_t kMaxBytes = 256;
        const auto name = callee->getName();

        if ((name == "memset" || name == "__memset") && call->arg_size() >= 3) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto val = getIntExpr(call->getArgOperand(1), cur, pred);
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              const Value *obj = getObjectForPtr(call->getArgOperand(0));
              if (obj && m_obj_mem.count(obj)) {
                const auto base = m_obj_base[obj].value();
                const auto off = dst - base;
                z3::expr b = val;
                const unsigned bw = b.get_sort().bv_size();
                if (bw < 8)
                  b = z3::zext(b, 8 - bw);
                else if (bw > 8)
                  b = b.extract(7, 0);
                z3::expr curMem = m_obj_mem[obj].value();
                for (uint64_t i = 0; i < *len; ++i) {
                  curMem = z3::store(
                      curMem,
                      off + m_solver.value().ctx().bv_val(i, m_ptr_bits), b);
                }
                m_obj_mem[obj] = curMem;
              } else {
                m_smt_mem->memsetBytes(dst, val, *len);
              }
            } else {
              const Value *obj = getObjectForPtr(call->getArgOperand(0));
              if (obj && !havocObjectAtPtr(obj, call->getArgOperand(0), *len,
                                           "memset_large")) {
                havocObject(obj, "memset_large");
              } else if (!obj) {
                m_smt_mem->havoc("memset_large");
              }
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
          } else {
            const Value *obj = getObjectForPtr(call->getArgOperand(0));
            if (obj)
              havocObject(obj, "memset_sym");
            else
              m_smt_mem->havoc("memset_sym");
          }
          // memset returns dst.
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        if ((name == "memcpy" || name == "__memcpy") && call->arg_size() >= 3) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto src = getPtrExpr(call->getArgOperand(1), cur, pred);
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              const Value *dstObj = getObjectForPtr(call->getArgOperand(0));
              const Value *srcObj = getObjectForPtr(call->getArgOperand(1));
              if (dstObj && srcObj && m_obj_mem.count(dstObj) &&
                  m_obj_mem.count(srcObj)) {
                const auto dstBase = m_obj_base[dstObj].value();
                const auto srcBase = m_obj_base[srcObj].value();
                z3::expr dstOff = dst - dstBase;
                z3::expr srcOff = src - srcBase;
                z3::expr dstMem = m_obj_mem[dstObj].value();
                z3::expr srcMem = m_obj_mem[srcObj].value();
                for (uint64_t i = 0; i < *len; ++i) {
                  z3::expr b = z3::select(
                      srcMem,
                      srcOff + m_solver.value().ctx().bv_val(i, m_ptr_bits));
                  dstMem = z3::store(
                      dstMem,
                      dstOff + m_solver.value().ctx().bv_val(i, m_ptr_bits), b);
                }
                m_obj_mem[dstObj] = dstMem;
              } else if (dstObj) {
                if (!havocObjectAtPtr(dstObj, call->getArgOperand(0), *len,
                                      "memcpy_unknown_src")) {
                  havocObject(dstObj, "memcpy_unknown_src");
                }
              } else {
                m_smt_mem->memcpyBytes(dst, src, *len);
              }
            } else {
              const Value *obj = getObjectForPtr(call->getArgOperand(0));
              if (obj && !havocObjectAtPtr(obj, call->getArgOperand(0), *len,
                                           "memcpy_large")) {
                havocObject(obj, "memcpy_large");
              } else if (!obj) {
                m_smt_mem->havoc("memcpy_large");
              }
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
          } else {
            const Value *obj = getObjectForPtr(call->getArgOperand(0));
            if (obj)
              havocObject(obj, "memcpy_sym");
            else
              m_smt_mem->havoc("memcpy_sym");
          }
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        if ((name == "memmove" || name == "__memmove") &&
            call->arg_size() >= 3) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto src = getPtrExpr(call->getArgOperand(1), cur, pred);
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              const Value *dstObj = getObjectForPtr(call->getArgOperand(0));
              const Value *srcObj = getObjectForPtr(call->getArgOperand(1));
              if (dstObj && srcObj && m_obj_mem.count(dstObj) &&
                  m_obj_mem.count(srcObj)) {
                const auto dstBase = m_obj_base[dstObj].value();
                const auto srcBase = m_obj_base[srcObj].value();
                z3::expr dstOff = dst - dstBase;
                z3::expr srcOff = src - srcBase;
                z3::expr dstMem = m_obj_mem[dstObj].value();
                z3::expr srcMem = m_obj_mem[srcObj].value();
                for (uint64_t i = 0; i < *len; ++i) {
                  z3::expr b = z3::select(
                      srcMem,
                      srcOff + m_solver.value().ctx().bv_val(i, m_ptr_bits));
                  dstMem = z3::store(
                      dstMem,
                      dstOff + m_solver.value().ctx().bv_val(i, m_ptr_bits), b);
                }
                m_obj_mem[dstObj] = dstMem;
              } else if (dstObj) {
                if (!havocObjectAtPtr(dstObj, call->getArgOperand(0), *len,
                                      "memmove_unknown_src")) {
                  havocObject(dstObj, "memmove_unknown_src");
                }
              } else {
                m_smt_mem->memmoveBytes(dst, src, *len);
              }
            } else {
              const Value *obj = getObjectForPtr(call->getArgOperand(0));
              if (obj && !havocObjectAtPtr(obj, call->getArgOperand(0), *len,
                                           "memmove_large")) {
                havocObject(obj, "memmove_large");
              } else if (!obj) {
                m_smt_mem->havoc("memmove_large");
              }
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
          } else {
            const Value *obj = getObjectForPtr(call->getArgOperand(0));
            if (obj)
              havocObject(obj, "memmove_sym");
            else
              m_smt_mem->havoc("memmove_sym");
          }
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        if (!callee->isDeclaration() &&
            !(name == "malloc" || name == "calloc" || name == "realloc" ||
              name == "free" || name == "kmalloc" || name == "kzalloc" ||
              name == "vmalloc")) {
          const SummaryAvailability summaryStatus =
              applySummary(call, cur, pred);
          if (summaryStatus == SummaryAvailability::Available) {
            if (m_solver.value().check() == z3::unsat)
              return;
            continue;
          }
          if (m_building_summary &&
              summaryStatus == SummaryAvailability::Unsupported) {
            m_summary_failed = true;
            if (m_summary_failure_reason.empty())
              m_summary_failure_reason =
                  "unsupported nested direct call in summary build";
            return;
          }
          if (summaryStatus == SummaryAvailability::Unsupported &&
              InterprocSummaryMode == SummaryMode::Required) {
            MKINT_WARN() << "Falling back to conservative call modeling for "
                         << callee->getName();
          }
        }

        // Unknown call with memory side effects: conservatively havoc memory so
        // subsequent loads don't assume stale/zero contents.
        const bool is_allocator = isAllocatorLike(callee) || name == "free";
        if (m_building_summary && callee->isDeclaration() && !is_allocator &&
            !call->doesNotAccessMemory() && !call->onlyReadsMemory()) {
          m_summary_failed = true;
          if (m_summary_failure_reason.empty()) {
            m_summary_failure_reason =
                "unsupported external side-effecting call in summary";
          }
          return;
        }
        if (!is_allocator && !call->doesNotAccessMemory() &&
            !call->onlyReadsMemory()) {
          bool any = false;
          SmallPtrSet<const Value *, 8> touchedObjects;
          SmallPtrSet<const Value *, 8> touchedRoots;
          for (unsigned idx = 0; idx < call->arg_size(); ++idx) {
            const Value *arg = call->getArgOperand(idx);
            if (!arg || !arg->getType()->isPointerTy())
              continue;
            const Value *obj = getObjectForPtr(arg);
            if (!obj || m_obj_freed.contains(obj) ||
                !callMayModObject(call, obj)) {
              continue;
            }
            const Value *resolved = resolveAliasedObject(obj);
            if (resolved && touchedRoots.contains(resolved))
              continue;
            const uint64_t bytes = pointerAccessBytes(arg);
            if (bytes > 0 &&
                havocObjectAtPtr(obj, arg, bytes, "call_sidefx_field")) {
              touchedObjects.insert(obj);
              if (resolved)
                touchedRoots.insert(resolved);
              any = true;
            }
          }
          for (const auto *obj : m_obj_list) {
            if (touchedObjects.contains(obj) || m_obj_freed.contains(obj))
              continue;
            const Value *resolved = resolveAliasedObject(obj);
            if (resolved && touchedRoots.contains(resolved))
              continue;
            if (callMayModObject(call, obj)) {
              havocObject(obj, "call_sidefx");
              any = true;
            }
          }
          if (!any) {
            m_smt_mem->havoc("call_sidefx");
          }
        }

        if (name == "free" && call->arg_size() >= 1) {
          if (const Value *obj = getObjectForPtr(call->getArgOperand(0))) {
            invalidateObject(obj, "free");
          } else {
            m_smt_mem->havoc("free_unknown");
          }
          continue;
        }
      }

      // Model common allocators as fresh, disjoint heap objects.
      if (call->getType()->isPointerTy()) {
        Function *callee = call->getCalledFunction();
        if (callee) {
          const auto name = callee->getName();
          z3::expr sizeBytes = m_solver.value().ctx().bv_val(0, m_ptr_bits);
          bool sizeKnown = false;
          if (name == "malloc" || name == "kmalloc" || name == "kzalloc" ||
              name == "vmalloc") {
            if (call->arg_size() >= 1 &&
                call->getArgOperand(0)->getType()->isIntegerTy()) {
              sizeBytes = getIntExpr(call->getArgOperand(0), cur, pred);
              const unsigned abw = sizeBytes.get_sort().bv_size();
              if (abw < m_ptr_bits)
                sizeBytes = z3::zext(sizeBytes, m_ptr_bits - abw);
              if (abw > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
            }
            sizeKnown = sizeBytes.is_numeral();
          } else if (name == "calloc") {
            if (call->arg_size() >= 2 &&
                call->getArgOperand(0)->getType()->isIntegerTy() &&
                call->getArgOperand(1)->getType()->isIntegerTy()) {
              auto n = getIntExpr(call->getArgOperand(0), cur, pred);
              auto m = getIntExpr(call->getArgOperand(1), cur, pred);
              const unsigned n_bw = n.get_sort().bv_size();
              const unsigned m_bw = m.get_sort().bv_size();
              const unsigned target =
                  std::max(std::max(n_bw, m_bw), m_ptr_bits);
              if (n_bw < target)
                n = z3::zext(n, target - n_bw);
              if (m_bw < target)
                m = z3::zext(m, target - m_bw);
              sizeBytes = (n * m);
              if (target > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
            }
            sizeKnown = sizeBytes.is_numeral();
          } else if (name == "realloc") {
            if (call->arg_size() >= 2 &&
                call->getArgOperand(1)->getType()->isIntegerTy()) {
              sizeBytes = getIntExpr(call->getArgOperand(1), cur, pred);
              const unsigned abw = sizeBytes.get_sort().bv_size();
              if (abw < m_ptr_bits)
                sizeBytes = z3::zext(sizeBytes, m_ptr_bits - abw);
              if (abw > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
            }
            sizeKnown = sizeBytes.is_numeral();
          }

          if (name == "malloc" || name == "kmalloc" || name == "kzalloc" ||
              name == "vmalloc" || name == "calloc" || name == "realloc") {
            if (name == "realloc" && call->arg_size() >= 1) {
              if (const Value *oldObj = getObjectForPtr(call->getArgOperand(0))) {
                invalidateObject(oldObj, "realloc_old");
              }
            }
            ensureObject(call,
                         ("heap." + cur->getParent()->getName().str() + "." +
                          std::to_string((uintptr_t)call)),
                         sizeBytes, sizeKnown);
            setSym(call, m_obj_base[call].value());
            continue;
          }
        }

        // Unknown pointer-returning call: treat as fresh pointer value.
        setSym(call, getPtrExpr(call, cur, pred));
        continue;
      }

      // Inline asm return value can be treated as unknown.
      if (call->getType()->isIntegerTy() && call->isInlineAsm()) {
        auto v = getIntExpr(call, cur, pred);
        if (m_robust_universal_inline_asm) {
          registerUniversal(v);
        }
        continue;
      }
    }

    // Integer SSA: keep existing bug checks, but also allow values derived from
    // loads, selects, etc.
    if (inst.getType()->isIntegerTy()) {
      if (auto *op = dyn_cast<BinaryOperator>(&inst)) {
        (void)getIntExpr(op->getOperand(0), cur, pred);
        (void)getIntExpr(op->getOperand(1), cur, pred);
        if (!m_building_summary) {
          m_bug_detection->binary_check(
              op, m_solver.value(), m_v2sym, m_overflow_insts,
              m_bad_shift_insts, m_div_zero_insts, m_robust_reachability,
              &m_path_constraints, &m_universal_vars,
              [this, op](interr type, const z3::expr &q) {
                dumpEfConstraint(op, type, q);
              },
              [this](interr type) { return isRobustBugEnabled(type); });
        }
        if (!addWellDefinedConstraints(op, cur, pred))
          return;
        const auto r =
            m_bug_detection->binary_op_propagate(op, m_v2sym, m_solver.value());
        setSym(op, r);
      } else if (auto *op = dyn_cast<CastInst>(&inst)) {
        if (isa<PtrToIntInst>(op)) {
          const auto r = getIntExpr(op, cur, pred);
          setSym(op, r);
        } else {
          (void)getValueExpr(op->getOperand(0), cur, pred);
          const auto r = m_bug_detection->cast_op_propagate(op, m_v2sym,
                                                            m_solver.value());
          setSym(op, r);
        }
      } else {
        (void)getIntExpr(&inst, cur, pred);
      }
    } else if (inst.getType()->isPointerTy()) {
      (void)getPtrExpr(&inst, cur, pred);
    }
  }

  for (auto *succ : m_bbpaths[cur]) {
    m_solver.value().push();
    pushSymFrame();
    pushObjectFrame();
    m_smt_mem->push();
    pushConstraintFrame();
    // Record the path depth before recursing so we can restore it on return.
    const size_t pathDepthBefore = m_bug_detection->getCurrentPath().size();
    path_solving(succ, cur);
    // Restore the path to the depth it had before the recursive call.
    // This is symmetric with the addPathPoint calls inside path_solving.
    auto currentPath = m_bug_detection->getCurrentPath();
    while (currentPath.size() > pathDepthBefore)
      currentPath.pop_back();
    m_bug_detection->setCurrentPath(currentPath);
    m_smt_mem->pop();
    popSymFrame();
    popObjectFrame();
    popConstraintFrame();
    m_solver.value().pop();
  }
}

} // namespace kint
