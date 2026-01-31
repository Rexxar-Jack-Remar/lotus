//===-- Verification/Sifa/Summarizers/ReUseSupersetCallSummarizer.h --------===//
//
// Call summarizer that re-uses summaries when input ⊆ knownInput (Ultimate-aligned).
//
// Uses SummaryCache per callee; reUseOrCompute: if ∃ cached (knownInput, summary)
// with leq(input, knownInput), return meet of such summaries; else compute and cache.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_REUSESUPERSETCALLSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_REUSESUPERSETCALLSUMMARIZER_H

#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/ICallSummarizer.h"
#include "Verification/Sifa/Summarizers/SummaryCache.h"

#include <unordered_map>

namespace lotus {
namespace sifa {

template <typename LabelT, typename StateT>
class ReUseSupersetCallSummarizer final : public ICallSummarizer<StateT> {
public:
  using Domain = AbstractDomain<LabelT, StateT>;

  ReUseSupersetCallSummarizer(const Domain &domain, ICallSummarizer<StateT> &inner)
      : domain_(domain), inner_(inner) {}

  /// Ultimate-aligned: optional stats for CALL_SUMMARIZER_OVERALL_TIME and APPLICATIONS.
  ReUseSupersetCallSummarizer(SifaStats &stats, const Domain &domain,
                              ICallSummarizer<StateT> &inner)
      : stats_(&stats), domain_(domain), inner_(inner) {}

  StateT summarize(const std::string &calleeName, const StateT &inputAfterCall) override {
    if (stats_) {
      stats_->start(SifaStats::Key::CALL_SUMMARIZER_OVERALL_TIME);
      stats_->increment(SifaStats::Key::CALL_SUMMARIZER_APPLICATIONS);
    }
    SummaryCache<StateT> &cache = perCalleeCache_[calleeName];
    StateT result = cache.reUseOrCompute(
        inputAfterCall,
        [this](const StateT &a, const StateT &b) { return domain_.leq(a, b); },
        [this, &calleeName, &inputAfterCall]() {
          return inner_.summarize(calleeName, inputAfterCall);
        },
        [this](const StateT &a, const StateT &b) { return domain_.meet(a, b); });
    if (stats_)
      stats_->stop(SifaStats::Key::CALL_SUMMARIZER_OVERALL_TIME);
    return result;
  }

private:
  SifaStats *stats_ = nullptr;
  const Domain &domain_;
  ICallSummarizer<StateT> &inner_;
  std::unordered_map<std::string, SummaryCache<StateT>> perCalleeCache_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_REUSESUPERSETCALLSUMMARIZER_H
