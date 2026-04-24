#include "Alias/UnificationBased/seadsa/InitializePasses.hh"

#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

namespace seadsa {

void initializeAnalysisPasses(llvm::PassRegistry &Registry) {
  // Initialize LLVM passes
  llvm::initializeTargetLibraryInfoWrapperPassPass(Registry);
  llvm::initializeCallGraphWrapperPassPass(Registry);
  llvm::initializeDominatorTreeWrapperPassPass(Registry);
  llvm::initializeLoopInfoWrapperPassPass(Registry);
}

} // namespace seadsa 