#include "Verification/Transform/PrepareOverflows.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace lotus {
namespace verification {
namespace transform {

llvm::PreservedAnalyses PrepareOverflowsPass::run(Function &F,
                                                  FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  FunctionCallee VerifierError = M->getOrInsertFunction(
      "__VERIFIER_error", FunctionType::get(Type::getVoidTy(Ctx), false));

  SmallVector<BinaryOperator *, 32> work;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *BO = dyn_cast<BinaryOperator>(&I);
      if (!BO)
        continue;
      if (!BO->getType()->isIntegerTy() || BO->getType()->isIntegerTy(1))
        continue;
      const unsigned Op = BO->getOpcode();
      if (Op == Instruction::Add || Op == Instruction::Sub ||
          Op == Instruction::Mul)
        work.push_back(BO);
    }
  }

  bool changed = false;
  for (BinaryOperator *BO : work) {
    IRBuilder<> B(BO);
    Intrinsic::ID IID = Intrinsic::sadd_with_overflow;
    if (BO->getOpcode() == Instruction::Sub)
      IID = Intrinsic::ssub_with_overflow;
    else if (BO->getOpcode() == Instruction::Mul)
      IID = Intrinsic::smul_with_overflow;

    Function *OF = Intrinsic::getDeclaration(M, IID, BO->getType());
    Value *Pair = B.CreateCall(OF, {BO->getOperand(0), BO->getOperand(1)});
    Value *Val = B.CreateExtractValue(Pair, 0);
    Value *Ov = B.CreateExtractValue(Pair, 1);

    Instruction *ThenTerm = SplitBlockAndInsertIfThen(Ov, BO, false);
    IRBuilder<> ThenB(ThenTerm);
    ThenB.CreateCall(VerifierError);
    ThenB.CreateUnreachable();
    ThenTerm->eraseFromParent();

    BO->replaceAllUsesWith(Val);
    BO->eraseFromParent();
    changed = true;
  }
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform
} // namespace verification
} // namespace lotus

namespace lotus {
namespace verification {
namespace transform {

PrepareOverflowsPass createPrepareOverflowsPass() {
  return PrepareOverflowsPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
