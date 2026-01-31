//===-- Verification/Sifa/Summarizers/InterpretCallSummarizer.h -----------===//
//
// Call summarizer that interprets the callee (ported from Ultimate Library-Sifa).
//
// Always computes a new call summary by interpreting the callee's path
// expression from entry to return with the given input.
// Ultimate-aligned: SifaStats CALL_SUMMARIZER_NEW_COMPUTATION_TIME, CACHE_MISSES.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_INTERPRETCALLSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_INTERPRETCALLSUMMARIZER_H

#include "Verification/Sifa/Caches/ProcedureResourceCache.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/ICallSummarizer.h"

#include "llvm/IR/Module.h"

namespace lotus {
namespace sifa {

template <typename StateT>
class InterpretCallSummarizer final : public ICallSummarizer<StateT> {
public:
  InterpretCallSummarizer(SifaStats &stats, const llvm::Module &M,
                          ProcedureResourceCache &cache,
                          DagInterpreter<Transition, StateT> &dagInterpreter)
      : stats_(stats), M_(M), cache_(cache), dagInterpreter_(dagInterpreter) {}

  StateT summarize(const std::string &calleeName, const StateT &inputAfterCall) override {
    const llvm::Function *callee = M_.getFunction(calleeName);
    if (!callee || callee->isDeclaration()) {
      return inputAfterCall; // external: assume identity
    }
    stats_.start(SifaStats::Key::CALL_SUMMARIZER_NEW_COMPUTATION_TIME);
    stats_.increment(SifaStats::Key::CALL_SUMMARIZER_CACHE_MISSES);

    const ProcedureResources &res = cache_.resourcesOf(*callee, {});
    StateT result = dagInterpreter_.interpretForSingleMarker(
        res.getRegexDag(), res.getDagOverlayPathToReturn(), inputAfterCall);

    stats_.stop(SifaStats::Key::CALL_SUMMARIZER_NEW_COMPUTATION_TIME);
    return result;
  }

private:
  SifaStats &stats_;
  const llvm::Module &M_;
  ProcedureResourceCache &cache_;
  DagInterpreter<Transition, StateT> &dagInterpreter_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_INTERPRETCALLSUMMARIZER_H
