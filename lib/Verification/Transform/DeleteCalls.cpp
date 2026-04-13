#include "Verification/Transform/DeleteCalls.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace llvm;

static cl::list<std::string>
    CallsToDelete("delete-call",
                  cl::desc("Specify which calls of functions to delete"),
                  cl::CommaSeparated);

namespace lotus {
namespace verification {
namespace transform {

llvm::PreservedAnalyses DeleteCallsPass::run(Function &F,
                                             FunctionAnalysisManager &) {
  bool changed = false;
  std::set<std::string> callsset{CallsToDelete.begin(), CallsToDelete.end()};

  if (callsset.empty())
    return PreservedAnalyses::all();

  for (auto &B : F) {
    for (auto it = B.begin(), et = B.end(); it != et;) {
      auto &I = *it++;
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;

      auto *op = CI->getCalledOperand()->stripPointerCasts();

      auto *fun = dyn_cast<Function>(op);
      if (!fun)
        continue;

      if (callsset.find(fun->getName().str()) != callsset.end()) {
        // remove the instruction
        I.replaceAllUsesWith(UndefValue::get(I.getType()));
        I.eraseFromParent();
        changed = true;
      }
    }
  }

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform
} // namespace verification
} // namespace lotus

namespace lotus {
namespace verification {
namespace transform {

DeleteCallsPass createDeleteCallsPass() { return DeleteCallsPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
