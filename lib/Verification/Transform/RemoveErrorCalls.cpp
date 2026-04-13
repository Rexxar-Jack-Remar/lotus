#include "Verification/Transform/RemoveErrorCalls.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

static cl::opt<bool> useExit("remove-error-calls-use-exit",
                             cl::desc("Insert __VERIFIER_exit(0) instead of "
                                      "__VERIFIER_assume(0)"),
                             cl::init(false));

namespace {

bool CloneMetadata(const Instruction *i1, Instruction *i2) {
  if (i1->hasMetadata()) {
    i2->setDebugLoc(i1->getDebugLoc());
    return true;
  }
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

llvm::PreservedAnalyses RemoveErrorCallsPass::run(Function &F,
                                                  FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  bool modified = false;
  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  std::unique_ptr<CallInst> ext;

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;) {
    Instruction *ins = &*I;
    ++I;
    if (CallInst *CI = dyn_cast<CallInst>(ins)) {
      if (CI->isInlineAsm())
        continue;

      const Value *val = CI->getCalledOperand()->stripPointerCasts();
      const Function *callee = dyn_cast<Function>(val);
      if (!callee || callee->isIntrinsic())
        continue;

      if (!callee->hasName())
        continue;

      StringRef name = callee->getName();

      if (name.equals("__VERIFIER_error") || name.equals("__assert_fail")) {
        if (!ext) {
          Type *argTy = Type::getInt32Ty(Ctx);
          auto extF = M->getOrInsertFunction(useExit ? "__VERIFIER_exit"
                                                     : "__VERIFIER_assume",
                                             Type::getVoidTy(Ctx), argTy);

          std::vector<Value *> args = {ConstantInt::get(argTy, 0)};
          ext = std::unique_ptr<CallInst>(CallInst::Create(extF, args));
        }

        // Insert __VERIFIER_assume(0), which aborts the path
        auto *CI2 = ext->clone();
        CloneMetadata(CI, CI2);
        CI2->insertAfter(CI);
        CI->eraseFromParent();

        modified = true;
      }
    }
  }
  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform
} // namespace verification
} // namespace lotus

namespace lotus {
namespace verification {
namespace transform {

RemoveErrorCallsPass createRemoveErrorCallsPass() {
  return RemoveErrorCallsPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
