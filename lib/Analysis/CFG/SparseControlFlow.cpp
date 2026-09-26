#include "Analysis/CFG/SparseControlFlow.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

namespace lotus {

namespace {

bool isNonPointerType(const llvm::Type *Ty) {
  if (const auto *Struct = llvm::dyn_cast<llvm::StructType>(Ty)) {
    for (const auto *ElemTy : Struct->elements()) {
      if (!ElemTy->isSingleValueType() || ElemTy->isVectorTy()) {
        return false;
      }
    }
    return true;
  }
  if (const auto *Vec = llvm::dyn_cast<llvm::VectorType>(Ty)) {
    return !Vec->getElementType()->isPointerTy();
  }
  return Ty->isSingleValueType();
}

bool isNonAddressTakenVariable(const llvm::Value *Val) {
  const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Val);
  if (!Alloca) {
    return false;
  }
  for (const auto &Use : Alloca->uses()) {
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(Use.getUser())) {
      if (Use == Store->getValueOperand()) {
        return false;
      }
    } else if (const auto *Call =
                   llvm::dyn_cast<llvm::CallBase>(Use.getUser())) {
      auto ArgNo = Use.getOperandNo();
      if (Call->paramHasAttr(ArgNo, llvm::Attribute::StructRet)) {
        continue;
      }
      if (Call->paramHasAttr(ArgNo, llvm::Attribute::NoCapture) &&
          isNonPointerType(Call->getType())) {
        continue;
      }
      return false;
    }
  }
  return true;
}

bool checkMayAlias(const llvm::Value *Ptr1, const llvm::Value *Ptr2,
                   const SparseLLVMControlFlow::AliasCheckFn &MayAlias) {
  if (Ptr1 == Ptr2 ||
      Ptr1->stripPointerCastsAndAliases() ==
          Ptr2->stripPointerCastsAndAliases()) {
    return true;
  }
  if (isNonAddressTakenVariable(Ptr1) || isNonAddressTakenVariable(Ptr2)) {
    return false;
  }
  if (MayAlias) {
    return MayAlias(Ptr1, Ptr2);
  }
  return false;
}

void buildSparseCFG(SparseLLVMBasedCFG::vgraph_t &SCFG,
                    const llvm::Function *Fun, const llvm::Value *Val,
                    const SparseLLVMControlFlow::AliasCheckFn &MayAlias) {
  if (!Fun || Fun->isDeclaration() || Fun->empty()) {
    return;
  }

  llvm::SmallVector<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>, 16>
      WL;

  const auto *Entry = &Fun->getEntryBlock().front();
  if (llvm::isa<llvm::DbgInfoIntrinsic>(Entry)) {
    Entry = Entry->getNextNonDebugInstruction();
  }
  if (!Entry) {
    return;
  }

  for (const auto *Succ : SparseLLVMControlFlow::getNormalSuccsOf(Entry)) {
    WL.emplace_back(Entry, Succ);
  }

  llvm::SmallDenseSet<const llvm::Instruction *, 32> Handled;

  while (!WL.empty()) {
    auto [From, To] = WL.pop_back_val();

    const auto *Curr = From;
    if (SparseLLVMControlFlow::shouldKeepInst(To, Val, MayAlias)) {
      Curr = To;
      auto [It, Inserted] = SCFG.try_emplace(From, To);
      if (!Inserted && It->second != To) {
        It->second = nullptr;
      }
    }

    if (!Handled.insert(To).second) {
      continue;
    }

    for (const auto *Succ : SparseLLVMControlFlow::getNormalSuccsOf(To)) {
      WL.emplace_back(Curr, Succ);
    }
  }
}

} // namespace

bool SparseLLVMControlFlow::isStartInst(const llvm::Instruction *Inst) {
  if (!Inst) {
    return false;
  }
  const auto *BB = Inst->getParent();
  if (!BB) {
    return false;
  }
  const auto *First = &BB->front();
  if (llvm::isa<llvm::DbgInfoIntrinsic>(First)) {
    First = First->getNextNonDebugInstruction();
  }
  return Inst == First;
}

bool SparseLLVMControlFlow::isExitInst(const llvm::Instruction *Inst) {
  return Inst &&
         (llvm::isa<llvm::ReturnInst>(Inst) ||
          llvm::isa<llvm::ResumeInst>(Inst) ||
          llvm::isa<llvm::UnreachableInst>(Inst));
}

bool SparseLLVMControlFlow::isNoopIntrinsic(const llvm::Instruction *Inst) {
  if (!Inst) {
    return false;
  }
  if (llvm::isa<llvm::DbgInfoIntrinsic>(Inst)) {
    return true;
  }
  const auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(Inst);
  return II && II->isAssumeLikeIntrinsic() && II->getType()->isVoidTy();
}

llvm::SmallVector<const llvm::Instruction *, 2>
SparseLLVMControlFlow::getNormalSuccsOf(const llvm::Instruction *Inst) {
  llvm::SmallVector<const llvm::Instruction *, 2> Succs;
  if (!Inst) {
    return Succs;
  }

  if (const auto *Next = Inst->getNextNonDebugInstruction()) {
    Succs.push_back(Next);
    return Succs;
  }

  const auto *BB = Inst->getParent();
  if (!BB) {
    return Succs;
  }

  for (const auto *SuccBB : llvm::successors(BB)) {
    const auto *First = &SuccBB->front();
    if (llvm::isa<llvm::DbgInfoIntrinsic>(First)) {
      First = First->getNextNonDebugInstruction();
    }
    if (First) {
      Succs.push_back(First);
    }
  }

  return Succs;
}

bool SparseLLVMControlFlow::shouldKeepInst(const llvm::Instruction *Inst,
                                           const llvm::Value *Val,
                                           const AliasCheckFn &MayAlias) {
  if (!Inst || !Val) {
    return true;
  }
  if (Inst == Val || isExitInst(Inst) || isStartInst(Inst)) {
    return true;
  }
  if (isNoopIntrinsic(Inst)) {
    return false;
  }
  if (llvm::isa<llvm::CallBase>(Inst) && llvm::isa<llvm::GlobalValue>(Val)) {
    return true;
  }

  const auto *ValTy = Val->getType();
  bool ValPtr = ValTy->isPointerTy();

  for (const auto *Op : Inst->operand_values()) {
    if (Op == Val) {
      return true;
    }
    if (!ValPtr) {
      continue;
    }
    if (!Op->getType()->isPointerTy()) {
      continue;
    }
    if (checkMayAlias(Val, Op, MayAlias)) {
      return true;
    }
  }

  return false;
}

const llvm::Instruction *
SparseLLVMControlFlow::advanceToNextUser(const llvm::Instruction *Succ,
                                         const llvm::Value *Fact,
                                         const AliasCheckFn &MayAlias) {
  if (!Succ || !Fact) {
    return Succ;
  }
  if (Succ == Fact || isExitInst(Succ) || isStartInst(Succ)) {
    return Succ;
  }
  if (llvm::isa<llvm::CallBase>(Succ) && !isNoopIntrinsic(Succ) &&
      llvm::isa<llvm::GlobalValue>(Fact)) {
    return Succ;
  }

  const auto *Save = Succ;
  while (!shouldKeepInst(Succ, Fact, MayAlias)) {
    const auto *NextSucc = Succ->getNextNonDebugInstruction();
    if (!NextSucc) {
      const auto *Parent = Succ->getParent();
      if (llvm::succ_size(Parent) == 1) {
        const auto *SuccBB = *llvm::succ_begin(Parent);
        Succ = &SuccBB->front();
        if (llvm::isa<llvm::DbgInfoIntrinsic>(Succ)) {
          Succ = Succ->getNextNonDebugInstruction();
        }
        if (Succ != Save && llvm::pred_size(SuccBB) == 1) {
          continue;
        }
        return Succ;
      }
      break;
    }
    Succ = NextSucc;
  }
  return Succ;
}

SparseLLVMBasedICFG::SparseLLVMBasedICFG(const CallGraph *CG,
                                         AliasCheckFn MayAlias)
    : CG(CG), MayAlias(std::move(MayAlias)) {}

const SparseLLVMBasedCFG &
SparseLLVMBasedICFG::getSparseCFG(const llvm::Function *Fun,
                                  const llvm::Value *Val) const {
  auto [It, Inserted] = Cache.try_emplace(std::make_pair(Fun, Val));
  if (Inserted) {
    buildSparseCFG(It->second.VGraph, Fun, Val, MayAlias);
  }
  return It->second;
}

const llvm::Instruction *
SparseLLVMBasedICFG::advanceToNextUser(const llvm::Instruction *Succ,
                                       const llvm::Value *Fact) const {
  return SparseLLVMControlFlow::advanceToNextUser(Succ, Fact, MayAlias);
}

llvm::SmallVector<const llvm::Instruction *, 2>
SparseLLVMBasedICFG::getSparseSuccsOf(const llvm::Instruction *Inst,
                                      const llvm::Value *Fact) const {
  llvm::SmallVector<const llvm::Instruction *, 2> Result;
  if (!Inst) {
    return Result;
  }

  if (Fact) {
    const auto &SCFG = getSparseCFG(Inst->getFunction(), Fact);
    if (const auto *UniqueNext = SCFG.nextUserOrNull(Inst)) {
      Result.push_back(UniqueNext);
      return Result;
    }
  }

  for (const auto *Succ : SparseLLVMControlFlow::getNormalSuccsOf(Inst)) {
    if (Fact) {
      Result.push_back(advanceToNextUser(Succ, Fact));
    } else {
      Result.push_back(Succ);
    }
  }
  return Result;
}

} // namespace lotus
