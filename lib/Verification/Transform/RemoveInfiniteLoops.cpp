#include "Verification/Transform/RemoveInfiniteLoops.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <vector>

using namespace llvm;

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

static bool isLoop(BasicBlock *block) {
  // Check if this is a block that unconditionally jumps on itself
  std::set<BasicBlock *> visited;
  visited.insert(block);
  auto *cur = block;
  do {
    for (auto &I : *cur) {
      if (I.mayWriteToMemory())
        return false;
      if (isa<CallInst>(&I))
        return false;
      if (isa<ReturnInst>(&I))
        return false;
    }

    cur = cur->getUniqueSuccessor();
    if (!cur) { // no loop
      return false;
    }

    if (!visited.insert(cur).second) {
      // hit a cycle
      return true;
    }
  } while (true);

  assert(false && "Should not be reachable");
  return false;
}

llvm::PreservedAnalyses RemoveInfiniteLoopsPass::run(
    Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  Module *M = F.getParent();
  std::vector<BasicBlock *> to_process;
  for (BasicBlock &block : F) {
    if (isLoop(&block))
      to_process.push_back(&block);
  }

  if (to_process.empty())
    return PreservedAnalyses::all();

  LLVMContext &Ctx = M->getContext();
  Type *argTy = Type::getInt32Ty(Ctx);
  auto C =
      M->getOrInsertFunction("__VERIFIER_assume", Type::getVoidTy(Ctx), argTy);
  auto *extF = cast<Function>(C.getCallee()->stripPointerCasts());

  std::vector<Value *> args = {ConstantInt::get(argTy, 0)};

  for (BasicBlock *block : to_process) {
    Instruction *T = block->getTerminator();
    auto *ext = CallInst::Create(extF, args);
    CloneMetadata(&*(block->begin()), ext);
    ext->insertBefore(T);

    // Replace the jump with unreachable,
    // since the assume(0) will terminate the computation
    new UnreachableInst(Ctx, T);
    T->eraseFromParent();
  }

  errs() << "Removed infinite loop in " << F.getName() << "\n";
  return PreservedAnalyses::none();
}

} // namespace transform
} // namespace verification
} // namespace lotus

namespace lotus {
namespace verification {
namespace transform {

RemoveInfiniteLoopsPass createRemoveInfiniteLoopsPass() {
  return RemoveInfiniteLoopsPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
