#include "Analysis/DebugInfo/UniqueIR/UniqueIRMarkerPass.h"

#include "Analysis/DebugInfo/UniqueIR/UniqueIRMarker.h"
#include "Analysis/DebugInfo/UniqueIR/UniqueIRVerifier.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Support/CommandLine.h>

namespace {

llvm::cl::opt<bool> instrument_ir("lotus-instrument-ir",
                                  llvm::cl::desc("Add unique Lotus IR IDs"));
llvm::cl::opt<bool> reinstrument_ir(
    "lotus-reinstrument-ir",
    llvm::cl::desc("Overwrite existing unique Lotus IR IDs"));
llvm::cl::opt<bool> renumber_ir(
    "lotus-renumber-ir",
    llvm::cl::desc("Renumber only IR values that already have unique IDs"));
llvm::cl::opt<bool> verify_ir("lotus-verify-ir",
                              llvm::cl::desc("Verify unique Lotus IR IDs"));

} // namespace

namespace lotus {

UniqueIRMarkerPass::UniqueIRMarkerPass() : llvm::ModulePass(ID) {}

bool UniqueIRMarkerPass::runOnModule(llvm::Module &module) {
  auto get_loop_info = [this](llvm::Function &function) -> llvm::LoopInfo * {
    if (function.empty()) {
      return nullptr;
    }
    return &getAnalysis<llvm::LoopInfoWrapperPass>(function).getLoopInfo();
  };

  if (verify_ir) {
    UniqueIRVerifier verifier;
    assert(verifier.verify(module, get_loop_info) &&
           "UniqueIRMarkerPass verification failed");
    return false;
  }

  if (!instrument_ir && !reinstrument_ir && !renumber_ir) {
    return false;
  }

  auto mode = instrument_ir ? UniqueIRMarkerMode::Instrument
                            : (renumber_ir ? UniqueIRMarkerMode::Renumber
                                           : UniqueIRMarkerMode::Reinstrument);
  UniqueIRMarker marker(mode);
  return marker.mark(module, get_loop_info);
}

void UniqueIRMarkerPass::getAnalysisUsage(
    llvm::AnalysisUsage &analysis_usage) const {
  analysis_usage.addRequired<llvm::LoopInfoWrapperPass>();
}

char UniqueIRMarkerPass::ID = 0;

static llvm::RegisterPass<UniqueIRMarkerPass> X(
    "lotus-unique-ir-id",
    "Add stable unique IDs to modules, functions, loops, basic blocks, and "
    "instructions");

} // namespace lotus
