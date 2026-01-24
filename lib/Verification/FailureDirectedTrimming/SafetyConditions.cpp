#include "FailureDirectedTrimmingImpl.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>

#include <functional>

using namespace llvm;

ExprRef storeOp(const ExprFactory &F, BoundVarManager &BVM,
                lotus::AliasAnalysisWrapper &AA, const Instruction *CtxI,
                uint64_t TagBase, ExprRef Ptr, Type *StoredValTy, ExprRef NewVal,
                ExprRef Phi) {
  if (!Ptr || !StoredValTy || !NewVal || !Phi)
    return Phi;

  DenseMap<Type *, uint32_t> MismatchIds;

  std::function<ExprRef(const ExprRef &)> Replace =
      [&](const ExprRef &E) -> ExprRef {
    if (!E)
      return nullptr;
    if (E->Kind == ExprKind::Deref) {
      if (E->Args.empty())
        return E;
      ExprRef KidPtr = Replace(E->Args[0]);
      if (KidPtr && exprEquals(KidPtr, Ptr)) {
        if (E->DerefValueTy == StoredValTy)
          return NewVal;
        if (CtxI && E->DerefValueTy) {
          uint32_t Id = BVM.getId(CtxI, TagBase, E->DerefValueTy);
          MismatchIds[E->DerefValueTy] = Id;
          return F.boundVar(Id, E->DerefValueTy);
        }
        return E;
      }
      return F.deref(KidPtr, E->DerefValueTy);
    }
    if (E->Args.empty())
      return E;

    std::vector<ExprRef> Kids;
    Kids.reserve(E->Args.size());
    for (auto &C : E->Args)
      Kids.push_back(Replace(C));

    switch (E->Kind) {
    case ExprKind::Not:
      return F.not_(Kids[0]);
    case ExprKind::And:
      return F.and_(Kids);
    case ExprKind::Or:
      return F.or_(Kids);
    case ExprKind::Add:
      return F.add(Kids[0], Kids[1]);
    case ExprKind::Sub:
      return F.sub(Kids[0], Kids[1]);
    case ExprKind::Mul:
      return F.mul(Kids[0], Kids[1]);
    case ExprKind::ICmp:
      return F.icmp(E->Pred, Kids[0], Kids[1]);
    case ExprKind::Deref:
      return F.deref(Kids[0], E->DerefValueTy);
    case ExprKind::Cast:
      return F.cast(E->CastOp, E->Ty, Kids[0]);
    case ExprKind::Gep: {
      SmallVector<ExprRef, 8> Idxs;
      for (size_t i = 1; i < Kids.size(); ++i)
        Idxs.push_back(Kids[i]);
      return F.gep(E->GepSourceEltTy, E->GepInBounds, Kids[0], Idxs, E->Ty);
    }
    case ExprKind::Forall:
      return F.forall(E->BoundId, E->DerefValueTy, Kids[0]);
    case ExprKind::Exists:
      return F.exists(E->BoundId, E->DerefValueTy, Kids[0]);
    case ExprKind::BoolConst:
    case ExprKind::IntConst:
    case ExprKind::Var:
    case ExprKind::BoundVar:
      return E;
    }
    return E;
  };

  ExprRef SubstPhi = Replace(Phi);

  std::vector<ExprRef> DerefLocs;
  collectDerefPtrs(Phi, DerefLocs);

  std::vector<ExprRef> Conjs;
  Conjs.push_back(SubstPhi);
  for (const ExprRef &Loc : DerefLocs) {
    if (!Loc || !Loc->Ty || !Loc->Ty->isPointerTy())
      continue;
    if (!Ptr->Ty || !Ptr->Ty->isPointerTy())
      continue;
    if (exprEquals(Loc, Ptr))
      continue;
    if (!mayAliasPtrExpr(Loc, Ptr, AA))
      continue;
    ExprRef Ne = icmpPtr(F, CmpInst::ICMP_NE, Loc, Ptr);
    if (Ne)
      Conjs.push_back(Ne);
  }

  ExprRef Out = F.and_(Conjs);

  // If Φ contains deref(Ptr) terms of a different LLVM type than the current
  // store, we conservatively model those views as arbitrary.
  for (auto &KV : MismatchIds) {
    Type *Ty = KV.first;
    uint32_t Id = KV.second;
    Out = F.forall(Id, Ty, Out);
  }

  return Out;
}

// -----------------------------------------------------------------------------
// buildValueExpr, asBoolExpr, buildRhsExpr, havocVar, icmpPtr
// -----------------------------------------------------------------------------
ExprRef buildValueExpr(const ExprFactory &F, const Value *V) {
  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    return F.intConst(CI->getValue(), cast<IntegerType>(CI->getType()));
  }
  if (isa<ConstantPointerNull>(V)) {
    return F.var(V);
  }
  return F.var(V);
}

ExprRef asBoolExpr(const ExprFactory &F, ExprRef E) {
  if (!E || !E->Ty)
    return nullptr;
  if (E->Ty->isIntegerTy(1))
    return E;
  if (auto *IT = dyn_cast<IntegerType>(E->Ty)) {
    APInt Z(IT->getBitWidth(), 0);
    return F.icmp(CmpInst::ICMP_NE, E, F.intConst(Z, IT));
  }
  if (E->Ty->isPointerTy()) {
    auto *Null = ConstantPointerNull::get(cast<PointerType>(E->Ty));
    return F.icmp(CmpInst::ICMP_NE, E, F.var(Null));
  }
  return nullptr;
}

ExprRef buildRhsExpr(const ExprFactory &F, const Instruction *I) {
  if (auto *BO = dyn_cast<BinaryOperator>(I)) {
    if (!BO->getType()->isIntegerTy())
      return nullptr;
    ExprRef A = buildValueExpr(F, BO->getOperand(0));
    ExprRef B = buildValueExpr(F, BO->getOperand(1));
    switch (BO->getOpcode()) {
    case Instruction::Add:
      return F.add(A, B);
    case Instruction::Sub:
      return F.sub(A, B);
    case Instruction::Mul:
      return F.mul(A, B);
    default:
      return nullptr;
    }
  }

  if (auto *Cmp = dyn_cast<ICmpInst>(I)) {
    ExprRef A = buildValueExpr(F, Cmp->getOperand(0));
    ExprRef B = buildValueExpr(F, Cmp->getOperand(1));
    return F.icmp(Cmp->getPredicate(), A, B);
  }

  if (auto *LI = dyn_cast<LoadInst>(I)) {
    ExprRef Ptr = buildValueExpr(F, LI->getPointerOperand());
    return F.deref(Ptr, LI->getType());
  }

  if (auto *CI = dyn_cast<CastInst>(I)) {
    ExprRef Src = buildValueExpr(F, CI->getOperand(0));
    return F.cast(static_cast<Instruction::CastOps>(CI->getOpcode()),
                  CI->getType(), Src);
  }

  if (auto *G = dyn_cast<GetElementPtrInst>(I)) {
    ExprRef Base = buildValueExpr(F, G->getPointerOperand());
    SmallVector<ExprRef, 8> Idxs;
    for (const auto *IdxIt = G->idx_begin(); IdxIt != G->idx_end(); ++IdxIt) {
      Idxs.push_back(buildValueExpr(F, *IdxIt));
    }
    return F.gep(G->getSourceElementType(), G->isInBounds(), Base, Idxs,
                 G->getType());
  }

  return nullptr;
}

ExprRef havocVar(const ExprFactory &F, BoundVarManager &BVM,
                 const Instruction *I, uint64_t Tag, Type *Ty, const Value *V,
                 ExprRef Phi) {
  uint32_t Id = BVM.getId(I, Tag, Ty);
  ExprRef BV = F.boundVar(Id, Ty);
  Subst S;
  S.Vars[V] = BV;
  ExprRef Body = substitute(F, Phi, S);
  return F.forall(Id, Ty, Body);
}

ExprRef icmpPtr(const ExprFactory &F, CmpInst::Predicate Pred, ExprRef A,
                ExprRef B) {
  if (!A || !B)
    return nullptr;
  if (!A->Ty || !B->Ty)
    return nullptr;
  if (!A->Ty->isPointerTy() || !B->Ty->isPointerTy() || A->Ty == B->Ty)
    return F.icmp(Pred, A, B);

  auto *PTA = cast<PointerType>(A->Ty);
  auto *PTB = cast<PointerType>(B->Ty);
  if (PTA->getAddressSpace() != PTB->getAddressSpace()) {
    if (Pred == CmpInst::ICMP_EQ)
      return F.boolConst(false);
    if (Pred == CmpInst::ICMP_NE)
      return F.boolConst(true);
    return nullptr;
  }

  Type *I8PtrTy = Type::getInt8PtrTy(F.Ctx, PTA->getAddressSpace());
  ExprRef A2 = F.cast(Instruction::BitCast, I8PtrTy, A);
  ExprRef B2 = F.cast(Instruction::BitCast, I8PtrTy, B);
  return F.icmp(Pred, A2, B2);
}

// -----------------------------------------------------------------------------
// storeTransfer, havocDerefLocation, summaryOf, callTransfer
// -----------------------------------------------------------------------------
ExprRef storeTransfer(const ExprFactory &F, BoundVarManager &BVM,
                      lotus::AliasAnalysisWrapper &AA,
                      const Instruction *I, const StoreInst *SI, ExprRef Phi) {
  ExprRef Ptr = buildValueExpr(F, SI->getPointerOperand());
  ExprRef Val = buildValueExpr(F, SI->getValueOperand());
  uint64_t TagBase = llvm::hash_combine(reinterpret_cast<uintptr_t>(SI),
                                        reinterpret_cast<uintptr_t>(SI->getPointerOperand()),
                                        reinterpret_cast<uintptr_t>(SI->getValueOperand()->getType()));
  return storeOp(F, BVM, AA, I, /*TagBase=*/300 + TagBase, Ptr,
                 SI->getValueOperand()->getType(), Val, Phi);
}

ExprRef havocDerefLocation(const ExprFactory &F, BoundVarManager &BVM,
                           lotus::AliasAnalysisWrapper &AA,
                           const Instruction *I, uint64_t Tag, ExprRef Ptr,
                           Type *ValTy, ExprRef Phi) {
  if (!Ptr || !ValTy)
    return Phi;

  uint32_t Id = BVM.getId(I, Tag, ValTy);
  ExprRef BV = F.boundVar(Id, ValTy);
  ExprRef Body = storeOp(F, BVM, AA, I, /*TagBase=*/Tag, Ptr, ValTy, BV, Phi);
  return F.forall(Id, ValTy, Body);
}

ExprRef summaryOf(const ExprFactory &F, const SummaryEnv &Env,
                  const HasAsrtsEnv &Has, const Function *Callee,
                  ArrayRef<ExprRef> Actuals) {
  if (!Callee)
    return F.boolConst(false);

  auto It = Env.Summaries.find(Callee);
  if (It != Env.Summaries.end()) {
    ExprRef Sum = It->second;
    Subst S;
    unsigned idx = 0;
    for (auto &Arg : Callee->args()) {
      if (idx < Actuals.size())
        S.Vars[&Arg] = Actuals[idx];
      ++idx;
    }
    return substitute(F, Sum, S);
  }

  auto It2 = Has.HasAsrts.find(Callee);
  bool CalleeHasAsrts = (It2 != Has.HasAsrts.end()) ? It2->second : false;
  return CalleeHasAsrts ? F.boolConst(false) : F.boolConst(true);
}

ExprRef callTransfer(const ExprFactory &F, BoundVarManager &BVM,
                     lotus::AliasAnalysisWrapper &AA, const SummaryEnv &Env,
                     const HasAsrtsEnv &Has, const CallBase *CB, ExprRef Phi) {
  if (!CB)
    return Phi;
  const Function *Callee = getDirectCalledFunctionMatchingType(*CB);

  ExprRef Out = Phi;
  if (!CB->getType()->isVoidTy()) {
    Out = havocVar(F, BVM, CB, /*Tag=*/1, CB->getType(), CB, Out);
  }

  if (CB->mayWriteToMemory()) {
    std::vector<ExprRef> Derefs;
    collectDerefNodes(Out, Derefs);

    SmallVector<const Value *, 8> PtrArgs;
    for (auto &A : CB->args()) {
      if (A->getType()->isPointerTy())
        PtrArgs.push_back(A.get());
    }

    bool ArgMemOnly = CB->onlyAccessesArgMemory();
    bool HavocAll = !ArgMemOnly;

    struct HavocLoc {
      ExprRef Ptr;
      Type *ValTy = nullptr;
      uint64_t Tag = 0;
    };

    DenseSet<uint64_t> Seen;
    std::vector<HavocLoc> Locs;
    Locs.reserve(Derefs.size());

    auto dependsOnPtrArgs = [&](const ExprRef &PtrExpr) -> bool {
      if (!PtrExpr)
        return true;
      if (exprContainsKind(PtrExpr, ExprKind::BoundVar))
        return true;
      if (PtrArgs.empty())
        return false;
      SmallVector<const Value *, 8> Vars;
      collectPointerVars(PtrExpr, Vars);
      if (Vars.empty())
        return true;
      for (const Value *V : Vars) {
        if (!V || !V->getType()->isPointerTy())
          continue;
        for (const Value *PA : PtrArgs) {
          if (AA.mayAlias(V, PA))
            return true;
        }
      }
      return false;
    };

    for (const ExprRef &D : Derefs) {
      if (!D || D->Kind != ExprKind::Deref || D->Args.empty())
        continue;
      ExprRef PtrExpr = D->Args[0];
      Type *ValTy = D->DerefValueTy;
      if (!PtrExpr || !ValTy)
        continue;

      if (!HavocAll && !dependsOnPtrArgs(PtrExpr))
        continue;

      uint64_t Tag = llvm::hash_combine(llvm::hash_value(exprToString(PtrExpr)),
                                        reinterpret_cast<uintptr_t>(ValTy));
      if (Seen.insert(Tag).second)
        Locs.push_back({PtrExpr, ValTy, Tag});
    }

    for (const HavocLoc &L : Locs) {
      Out = havocDerefLocation(F, BVM, AA, CB, /*Tag=*/100 + L.Tag, L.Ptr,
                               L.ValTy, Out);
    }
  }

  SmallVector<ExprRef, 8> Actuals;
  for (auto &A : CB->args())
    Actuals.push_back(buildValueExpr(F, A.get()));

  ExprRef Sum = summaryOf(F, Env, Has, Callee, Actuals);
  return F.and_({Sum, Out});
}

// -----------------------------------------------------------------------------
// firstNonPhiNonDbg, edgePre, computeSafetyConditions, computeHasAsrts
// -----------------------------------------------------------------------------
Instruction *firstNonPhiNonDbg(BasicBlock &B) {
  for (Instruction &I : B) {
    if (isa<PHINode>(&I))
      continue;
    if (isa<DbgInfoIntrinsic>(&I))
      continue;
    return &I;
  }
  return B.getTerminator();
}

ExprRef edgePre(const ExprFactory &F, const SummaryEnv &Env,
                const HasAsrtsEnv &Has, const HasAsrtsEnv &,
                const DenseMap<const BasicBlock *, ExprRef> &PreAfterPhi,
                const BasicBlock *Succ, const BasicBlock *Pred) {
  auto It = PreAfterPhi.find(Succ);
  if (It == PreAfterPhi.end())
    return F.boolConst(true);
  ExprRef Phi = It->second;

  Subst S;
  for (const Instruction &I : *Succ) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    Value *In = PN->getIncomingValueForBlock(const_cast<BasicBlock *>(Pred));
    if (!In)
      continue;
    S.Vars[PN] = buildValueExpr(F, In);
  }
  return substitute(F, Phi, S);
}

FunctionSCResult computeSafetyConditions(
    Function &Fn, const ExprFactory &F, BoundVarManager &BVM,
    lotus::AliasAnalysisWrapper &AA, const SummaryEnv &Env,
    const HasAsrtsEnv &Has) {
  FunctionSCResult Res;

  DenseMap<const BasicBlock *, ExprRef> PreAfterPhi;
  for (BasicBlock &BB : Fn) {
    // Conservative initialization: safe to stop at any point.
    PreAfterPhi[&BB] = F.boolConst(false);
  }

  auto computePost = [&](BasicBlock &BB,
                         const DenseMap<const BasicBlock *, ExprRef> &CurPre)
      -> ExprRef {
    Instruction *Term = BB.getTerminator();
    if (!Term)
      return F.boolConst(true);

    if (isa<ReturnInst>(Term) || isa<UnreachableInst>(Term))
      return F.boolConst(true);

    // Invoke/callbr are terminators with call side effects.
    if (auto *CB = dyn_cast<CallBase>(Term)) {
      std::vector<ExprRef> All;
      for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
        All.push_back(
            edgePre(F, Env, Has, Has, CurPre, Term->getSuccessor(i), &BB));
      }
      ExprRef After = F.and_(All);

      Function *CF = getDirectCalledFunction(*CB);
      if (CF && (isAssumeFunctionName(CF->getName()) ||
                 isAssumeNotFunctionName(CF->getName()))) {
        if (CB->arg_size() >= 1) {
          ExprRef Arg0 = buildValueExpr(F, CB->getArgOperand(0));
          ExprRef Cond = asBoolExpr(F, Arg0);
          if (!Cond)
            Cond = F.boolConst(true);
          if (isAssumeNotFunctionName(CF->getName()))
            Cond = F.not_(Cond);
          return F.implies(Cond, After);
        }
        return After;
      }
      if (CF && isAssertFunctionName(CF->getName())) {
        if (CB->arg_size() >= 1) {
          ExprRef Arg0 = buildValueExpr(F, CB->getArgOperand(0));
          ExprRef Cond = asBoolExpr(F, Arg0);
          if (!Cond)
            Cond = F.boolConst(false);
          return F.and_({Cond, After});
        }
        return After;
      }
      if (CF && isErrorFunctionName(CF->getName()))
        return F.boolConst(false);

      return callTransfer(F, BVM, AA, Env, Has, CB, After);
    }

    if (auto *Br = dyn_cast<BranchInst>(Term)) {
      if (Br->isUnconditional()) {
        BasicBlock *Succ = Br->getSuccessor(0);
        return edgePre(F, Env, Has, Has, CurPre, Succ, &BB);
      }

      ExprRef Cond = buildValueExpr(F, Br->getCondition());
      BasicBlock *T = Br->getSuccessor(0);
      BasicBlock *E = Br->getSuccessor(1);
      ExprRef PreT = edgePre(F, Env, Has, Has, CurPre, T, &BB);
      ExprRef PreE = edgePre(F, Env, Has, Has, CurPre, E, &BB);
      return F.and_({F.implies(Cond, PreT), F.implies(F.not_(Cond), PreE)});
    }

    if (auto *Sw = dyn_cast<SwitchInst>(Term)) {
      ExprRef Cond = buildValueExpr(F, Sw->getCondition());
      std::vector<ExprRef> Conjs;
      Conjs.reserve(Sw->getNumSuccessors());

      for (auto &C : Sw->cases()) {
        ConstantInt *CaseVal = C.getCaseValue();
        BasicBlock *Succ = C.getCaseSuccessor();
        ExprRef CaseExpr = buildValueExpr(F, CaseVal);
        ExprRef Eq = F.icmp(CmpInst::ICMP_EQ, Cond, CaseExpr);
        Conjs.push_back(
            F.implies(Eq, edgePre(F, Env, Has, Has, CurPre, Succ, &BB)));
      }

      std::vector<ExprRef> NegCases;
      for (auto &C : Sw->cases()) {
        ExprRef CaseExpr = buildValueExpr(F, C.getCaseValue());
        NegCases.push_back(F.icmp(CmpInst::ICMP_NE, Cond, CaseExpr));
      }
      ExprRef NoneMatch = F.and_(NegCases);
      Conjs.push_back(F.implies(
          NoneMatch,
          edgePre(F, Env, Has, Has, CurPre, Sw->getDefaultDest(), &BB)));
      return F.and_(Conjs);
    }

    std::vector<ExprRef> All;
    for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
      All.push_back(
          edgePre(F, Env, Has, Has, CurPre, Term->getSuccessor(i), &BB));
    }
    return F.and_(All);
  };

  for (unsigned Iter = 0; Iter < FDTrimCFGIterations; ++Iter) {
    bool Changed = false;
    DenseMap<const BasicBlock *, ExprRef> NewPre = PreAfterPhi;

    for (BasicBlock &BB : Fn) {
      ExprRef Phi = computePost(BB, PreAfterPhi);

      Res.BeforeInst[BB.getTerminator()] = Phi;

      Instruction *Term = BB.getTerminator();
      for (auto It = Term->getIterator(); It != BB.begin();) {
        --It;
        Instruction *I = &*It;
        if (isa<PHINode>(I))
          break;

        if (auto *CB = dyn_cast<CallBase>(I)) {
          Function *CF = getDirectCalledFunction(*CB);
          if (CF && (isAssumeFunctionName(CF->getName()) ||
                     isAssumeNotFunctionName(CF->getName()))) {
            if (CB->arg_size() >= 1) {
              ExprRef Arg0 = buildValueExpr(F, CB->getArgOperand(0));
              ExprRef Cond = asBoolExpr(F, Arg0);
              if (!Cond)
                Cond = F.boolConst(true);
              if (isAssumeNotFunctionName(CF->getName()))
                Cond = F.not_(Cond);
              Phi = F.implies(Cond, Phi);
            }
          } else if (CF && isAssertFunctionName(CF->getName())) {
            if (CB->arg_size() >= 1) {
              ExprRef Arg0 = buildValueExpr(F, CB->getArgOperand(0));
              ExprRef Cond = asBoolExpr(F, Arg0);
              if (!Cond)
                Cond = F.boolConst(false);
              Phi = F.and_({Cond, Phi});
            }
          } else if (CF && isErrorFunctionName(CF->getName())) {
            Phi = F.boolConst(false);
          } else {
            Phi = callTransfer(F, BVM, AA, Env, Has, CB, Phi);
          }
        } else if (auto *RMW = dyn_cast<AtomicRMWInst>(I)) {
          ExprRef Ptr = buildValueExpr(F, RMW->getPointerOperand());
          Type *ValTy = RMW->getValOperand()->getType();
          uint64_t Tag = llvm::hash_combine(
              reinterpret_cast<uintptr_t>(RMW->getPointerOperand()),
              reinterpret_cast<uintptr_t>(ValTy));
          Phi = havocDerefLocation(F, BVM, AA, RMW, /*Tag=*/200 + Tag, Ptr,
                                   ValTy, Phi);
          Phi = havocVar(F, BVM, RMW, /*Tag=*/0, RMW->getType(), RMW, Phi);
        } else if (auto *CX = dyn_cast<AtomicCmpXchgInst>(I)) {
          ExprRef Ptr = buildValueExpr(F, CX->getPointerOperand());
          Type *ValTy = CX->getCompareOperand()->getType();
          uint64_t Tag = llvm::hash_combine(
              reinterpret_cast<uintptr_t>(CX->getPointerOperand()),
              reinterpret_cast<uintptr_t>(ValTy));
          Phi = havocDerefLocation(F, BVM, AA, CX, /*Tag=*/200 + Tag, Ptr,
                                   ValTy, Phi);
          Phi = havocVar(F, BVM, CX, /*Tag=*/0, CX->getType(), CX, Phi);
        } else if (auto *SI = dyn_cast<StoreInst>(I)) {
          Phi = storeTransfer(F, BVM, AA, I, SI, Phi);
        } else if (!I->getType()->isVoidTy()) {
          ExprRef Rhs = buildRhsExpr(F, I);
          if (!Rhs) {
            Phi = havocVar(F, BVM, I, /*Tag=*/0, I->getType(), I, Phi);
          } else {
            Subst S;
            S.Vars[I] = Rhs;
            Phi = substitute(F, Phi, S);
          }
        }

        Res.BeforeInst[I] = Phi;
      }

      Instruction *InsPt = firstNonPhiNonDbg(BB);
      NewPre[&BB] = Res.BeforeInst[InsPt];
    }

    for (auto &KV : NewPre) {
      const BasicBlock *BB = KV.first;
      ExprRef Old = PreAfterPhi[BB];
      if (exprToString(Old) != exprToString(KV.second)) {
        Changed = true;
        break;
      }
    }

    PreAfterPhi = std::move(NewPre);
    if (!Changed)
      break;
  }

  Res.PreAfterPhi = std::move(PreAfterPhi);
  Res.Summary = Res.PreAfterPhi.lookup(&Fn.getEntryBlock());
  return Res;
}

HasAsrtsEnv computeHasAsrts(Module &M) {
  HasAsrtsEnv Out;

  DenseMap<const Function *, bool> Direct;
  DenseMap<const Function *, SmallVector<const Function *, 8>> Calls;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    bool HasDirect = false;
    SmallVector<const Function *, 8> Callees;
    for (Instruction &I : instructions(F)) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      const Function *CF = getDirectCalledFunction(*CB);
      if (!CF)
        continue;
      if (isErrorFunctionName(CF->getName()) ||
          isAssertFunctionName(CF->getName()))
        HasDirect = true;
      if (!CF->isDeclaration())
        Callees.push_back(CF);
    }
    Direct[&F] = HasDirect;
    Calls[&F] = Callees;
  }

  for (auto &KV : Direct)
    Out.HasAsrts[KV.first] = KV.second;

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &KV : Calls) {
      const Function *F = KV.first;
      if (Out.HasAsrts[F])
        continue;
      for (const Function *C : KV.second) {
        if (Out.HasAsrts.lookup(C)) {
          Out.HasAsrts[F] = true;
          Changed = true;
          break;
        }
      }
    }
  }
  return Out;
}
