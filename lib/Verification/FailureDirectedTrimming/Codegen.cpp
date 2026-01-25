#include "FailureDirectedTrimmingImpl.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

// Lower Expr formulas into LLVM IR Values that can be passed to verifier.assume.
//
// A trimming condition must be a necessary condition for failure. When the
// condition cannot be represented precisely in LLVM IR (e.g., unresolved
// quantifiers), we conservatively avoid pruning by producing "true".

Value *codegenValue(const ExprFactory &F, const ExprRef &E, IRBuilder<> &B,
                    DenseMap<uint32_t, Value *> &BoundVals, Module &M,
                    NondetFactory &Nondet, DerefUFFactory &DerefUF) {
  if (!E)
    return nullptr;

  switch (E->Kind) {
  case ExprKind::BoolConst:
    return ConstantInt::get(Type::getInt1Ty(M.getContext()), E->BoolVal);
  case ExprKind::IntConst:
    return ConstantInt::get(cast<IntegerType>(E->Ty), E->IntVal);
  case ExprKind::Var:
    return const_cast<Value *>(E->VarVal);
  case ExprKind::BoundVar: {
    auto It = BoundVals.find(E->BoundId);
    if (It != BoundVals.end())
      return It->second;
    Value *V = Nondet.nondet(B, E->Ty);
    BoundVals[E->BoundId] = V;
    return V;
  }
  case ExprKind::Not: {
    Value *A = codegenValue(F, E->Args[0], B, BoundVals, M, Nondet, DerefUF);
    return B.CreateNot(A);
  }
  case ExprKind::And: {
    Value *Acc = ConstantInt::getTrue(M.getContext());
    for (auto &C : E->Args) {
      Value *V = codegenValue(F, C, B, BoundVals, M, Nondet, DerefUF);
      Acc = B.CreateAnd(Acc, V);
    }
    return Acc;
  }
  case ExprKind::Or: {
    Value *Acc = ConstantInt::getFalse(M.getContext());
    for (auto &C : E->Args) {
      Value *V = codegenValue(F, C, B, BoundVals, M, Nondet, DerefUF);
      Acc = B.CreateOr(Acc, V);
    }
    return Acc;
  }
  case ExprKind::Add: {
    Value *A = codegenValue(F, E->Args[0], B, BoundVals, M, Nondet, DerefUF);
    Value *C = codegenValue(F, E->Args[1], B, BoundVals, M, Nondet, DerefUF);
    return B.CreateAdd(A, C);
  }
  case ExprKind::Sub: {
    Value *A = codegenValue(F, E->Args[0], B, BoundVals, M, Nondet, DerefUF);
    Value *C = codegenValue(F, E->Args[1], B, BoundVals, M, Nondet, DerefUF);
    return B.CreateSub(A, C);
  }
  case ExprKind::Mul: {
    Value *A = codegenValue(F, E->Args[0], B, BoundVals, M, Nondet, DerefUF);
    Value *C = codegenValue(F, E->Args[1], B, BoundVals, M, Nondet, DerefUF);
    return B.CreateMul(A, C);
  }
  case ExprKind::ICmp: {
    Value *A = codegenValue(F, E->Args[0], B, BoundVals, M, Nondet, DerefUF);
    Value *C = codegenValue(F, E->Args[1], B, BoundVals, M, Nondet, DerefUF);
    return B.CreateICmp(E->Pred, A, C);
  }
  case ExprKind::Deref: {
    // Deref terms drf(ptr) appear due to modeling of loads and heap effects.
    // Their lowering is controlled by -fdtrim-deref-mode:
    //   - nondet: treat every dereference as an unconstrained value
    //   - load  : emit an actual LLVM load (may be unsound if ptr is invalid)
    //   - uf    : call an uninterpreted function drf_trim(ptr) to keep it pure
    Value *Ptr = codegenValue(F, E->Args[0], B, BoundVals, M, Nondet, DerefUF);
    Type *ValTy = E->DerefValueTy;
    if (!Ptr->getType()->isPointerTy()) {
      return Nondet.nondet(B, ValTy);
    }
    if (FDTrimDerefMode == "nondet") {
      return Nondet.nondet(B, ValTy);
    }
    if (FDTrimDerefMode == "load") {
      PointerType *ExpectedPtrTy = ValTy->getPointerTo();
      if (Ptr->getType() != ExpectedPtrTy) {
        Ptr = B.CreateBitCast(Ptr, ExpectedPtrTy);
      }
      return B.CreateLoad(ValTy, Ptr);
    }
    auto *PT = cast<PointerType>(Ptr->getType());
    unsigned AS = PT->getAddressSpace();
    Type *I8PtrTy = Type::getInt8PtrTy(M.getContext(), AS);
    if (Ptr->getType() != I8PtrTy) {
      Ptr = B.CreateBitCast(Ptr, I8PtrTy);
    }
    FunctionCallee Drf = DerefUF.get(ValTy, AS);
    return B.CreateCall(Drf, Ptr);
  }
  case ExprKind::Cast: {
    Value *Src = codegenValue(F, E->Args[0], B, BoundVals, M, Nondet, DerefUF);
    switch (E->CastOp) {
    case Instruction::Trunc:
      return B.CreateTrunc(Src, E->Ty);
    case Instruction::ZExt:
      return B.CreateZExt(Src, E->Ty);
    case Instruction::SExt:
      return B.CreateSExt(Src, E->Ty);
    case Instruction::BitCast:
      return B.CreateBitCast(Src, E->Ty);
    case Instruction::PtrToInt:
      return B.CreatePtrToInt(Src, E->Ty);
    case Instruction::IntToPtr:
      return B.CreateIntToPtr(Src, E->Ty);
    default:
      return Nondet.nondet(B, E->Ty);
    }
  }
  case ExprKind::Gep: {
    Value *Base = codegenValue(F, E->Args[0], B, BoundVals, M, Nondet, DerefUF);
    if (!Base->getType()->isPointerTy()) {
      return Nondet.nondet(B, E->Ty);
    }
    SmallVector<Value *, 8> Idxs;
    for (size_t i = 1; i < E->Args.size(); ++i) {
      Value *Idx =
          codegenValue(F, E->Args[i], B, BoundVals, M, Nondet, DerefUF);
      Idxs.push_back(Idx);
    }
    if (E->GepInBounds)
      return B.CreateInBoundsGEP(E->GepSourceEltTy, Base, Idxs);
    return B.CreateGEP(E->GepSourceEltTy, Base, Idxs);
  }
  case ExprKind::Forall:
  case ExprKind::Exists:
    // Trimming assumptions must be a necessary condition for failure.
    // If quantifiers remain at this stage, conservatively keep all paths by
    // treating them as true (i.e., do not prune based on an unresolved binder).
    return ConstantInt::getTrue(M.getContext());
  }
  return nullptr;
}

ExprRef eliminateExistsByNondet(const ExprFactory &F, const ExprRef &E,
                                IRBuilder<> &B, Module &M, NondetFactory &Nondet,
                                DenseMap<uint32_t, Value *> &BoundVals) {
  // Eliminates existential quantifiers by choosing a nondet witness value.
  //
  // This is the key step that makes trimming conditions executable: after
  // negating safety conditions, existentials often appear (due to havoc being
  // represented with forall). We turn ∃x.φ(x) into φ(w) where w is a fresh
  // nondeterministic SSA value.
  if (!E)
    return nullptr;
  if (E->Kind == ExprKind::Exists) {
    if (!BoundVals.count(E->BoundId)) {
      BoundVals[E->BoundId] = Nondet.nondet(B, E->DerefValueTy);
    }
    Subst S;
    S.Bound[E->BoundId] = F.var(BoundVals[E->BoundId]);
    ExprRef Body = substitute(F, E->Args[0], S);
    return eliminateExistsByNondet(F, Body, B, M, Nondet, BoundVals);
  }

  if (E->Args.empty())
    return E;

  std::vector<ExprRef> Kids;
  Kids.reserve(E->Args.size());
  for (auto &C : E->Args) {
    Kids.push_back(eliminateExistsByNondet(F, C, B, M, Nondet, BoundVals));
  }

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
  case ExprKind::Exists:
  case ExprKind::BoolConst:
  case ExprKind::IntConst:
  case ExprKind::Var:
  case ExprKind::BoundVar:
    return E;
  }
  return E;
}
