#include "FailureDirectedTrimmingImpl.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>

using namespace llvm;

bool shouldCloneSafeVersion(const Function &F) {
  if (F.isDeclaration())
    return false;
  StringRef Name = F.getName();
  if (Name.startswith("verifier.") || Name.startswith("__CRAB_") ||
      Name.startswith("__VERIFIER_") || Name.startswith("llvm."))
    return false;
  if (isAssumeFunctionName(Name) || isErrorFunctionName(Name) ||
      isAssumeNotFunctionName(Name) || isAssertFunctionName(Name) ||
      isNondetFunctionName(Name))
    return false;
  if (Name.endswith(".fdtrim.safe"))
    return false;
  return true;
}

DenseMap<Function *, Function *> cloneSafeFunctions(Module &M,
                                                    FunctionCallee AssumeFn) {
  DenseMap<Function *, Function *> SafeOf;

  for (Function &F : M) {
    if (!shouldCloneSafeVersion(F))
      continue;
    ValueToValueMapTy VMap;
    Function *Clone = CloneFunction(&F, VMap);
    Clone->setName(F.getName() + ".fdtrim.safe");
    SafeOf[&F] = Clone;
  }

  for (auto &KV : SafeOf) {
    Function *Safe = KV.second;

    std::vector<CallInst *> CallsToRewrite;
    for (Instruction &I : instructions(Safe)) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (!getDirectCalledFunctionMatchingType(*CI))
          continue;
        CallsToRewrite.push_back(CI);
      }
    }

    for (CallInst *CI : CallsToRewrite) {
      Function *CF = getDirectCalledFunctionMatchingType(*CI);
      if (!CF)
        continue;

      if (isErrorFunctionName(CF->getName())) {
        IRBuilder<> B(CI);
        Value *False = ConstantInt::getFalse(M.getContext());
        B.CreateCall(AssumeFn, False);
        CI->eraseFromParent();
        continue;
      }

      if (isAssertFunctionName(CF->getName())) {
        IRBuilder<> B(CI);
        Value *CondV = nullptr;
        if (CI->arg_size() >= 1)
          CondV = CI->getArgOperand(0);
        if (!CondV) {
          CondV = ConstantInt::getTrue(M.getContext());
        } else if (CondV->getType()->isIntegerTy(1)) {
          // ok
        } else if (CondV->getType()->isIntegerTy()) {
          CondV = B.CreateICmpNE(CondV, ConstantInt::get(CondV->getType(), 0));
        } else if (CondV->getType()->isPointerTy()) {
          CondV = B.CreateICmpNE(
              CondV,
              ConstantPointerNull::get(cast<PointerType>(CondV->getType())));
        } else {
          CondV = ConstantInt::getFalse(M.getContext());
        }
        B.CreateCall(AssumeFn, CondV);
        CI->eraseFromParent();
        continue;
      }

      if (isAssumeFunctionName(CF->getName()) ||
          isAssumeNotFunctionName(CF->getName()) ||
          isNondetFunctionName(CF->getName()))
        continue;

      for (auto &MapEntry : SafeOf) {
        if (CF == MapEntry.first) {
          CI->setCalledFunction(MapEntry.second);
          break;
        }
      }
    }
  }

  return SafeOf;
}

bool wrapCallsInOriginalFunctions(Module &M, FunctionCallee AssumeFn,
                                  DenseMap<Function *, Function *> &SafeOf,
                                  NondetFactory &Nondet) {
  bool Changed = false;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.getName().endswith(".fdtrim.safe"))
      continue;

    std::vector<CallInst *> Calls;
    for (Instruction &I : instructions(F)) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      Function *CF = getDirectCalledFunctionMatchingType(*CI);
      if (!CF)
        continue;
      if (isAssumeFunctionName(CF->getName()) ||
          isAssumeNotFunctionName(CF->getName()) ||
          isAssertFunctionName(CF->getName()) ||
          isErrorFunctionName(CF->getName()) || isNondetFunctionName(CF->getName()))
        continue;
      if (!SafeOf.count(CF))
        continue;
      Calls.push_back(CI);
    }

    std::reverse(Calls.begin(), Calls.end());

    for (CallInst *CI : Calls) {
      if (!CI->getParent())
        continue;
      Function *CF = CI->getCalledFunction();
      if (!CF || !SafeOf.count(CF))
        continue;

      BasicBlock *OrigBB = CI->getParent();
      Function *Fn = OrigBB->getParent();

      BasicBlock *ContBB =
          OrigBB->splitBasicBlock(CI->getIterator(), "fdtrim.cont");
      OrigBB->getTerminator()->eraseFromParent();

      LLVMContext &Ctx = M.getContext();
      BasicBlock *SafeBB = BasicBlock::Create(Ctx, "fdtrim.safe", Fn, ContBB);
      BasicBlock *FailBB = BasicBlock::Create(Ctx, "fdtrim.fail", Fn, ContBB);

      IRBuilder<> B(OrigBB);
      Value *Nd = Nondet.nondetBool(B);
      B.CreateCondBr(Nd, SafeBB, FailBB);

      IRBuilder<> BS(SafeBB);
      SmallVector<Value *, 8> CallArgs;
      for (Use &U : CI->args())
        CallArgs.push_back(U.get());
      CallInst *SafeCall = BS.CreateCall(SafeOf[CF], CallArgs);
      SafeCall->setCallingConv(CI->getCallingConv());
      SafeCall->setTailCallKind(CI->getTailCallKind());
      SafeCall->setAttributes(CI->getAttributes());
      if (!CI->getType()->isVoidTy()) {
        CI->replaceAllUsesWith(SafeCall);
      }
      BS.CreateBr(ContBB);

      IRBuilder<> BF(FailBB);
      CallInst *OrigCall = BF.CreateCall(CF, CallArgs);
      OrigCall->setCallingConv(CI->getCallingConv());
      OrigCall->setTailCallKind(CI->getTailCallKind());
      OrigCall->setAttributes(CI->getAttributes());
      BF.CreateCall(AssumeFn, ConstantInt::getFalse(Ctx));
      BF.CreateUnreachable();

      CI->eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}
