//===-- Verification/Sifa/Summarizers/FixpointLoopSummarizer.h ------------===//
//
// Fixpoint loop summarizer with widening (ported from Ultimate Sifa).
//
// Summarizes (inner)* by iterating:
//   pre := input
//   post := interpret(inner, pre)
//   until post ⊑ pre (using subsetEq for possibly altered states), then return pre.
// Ultimate-aligned: cache (Star, input)->result, fixpoint iteration timeout.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_FIXPOINTLOOPSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_FIXPOINTLOOPSUMMARIZER_H

#include "Verification/Sifa/Caches/StarDagCache.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Fluid/IFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/RegexDag/FullOverlay.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/ILoopSummarizer.h"

#include <cstddef>
#include <functional>
#include <tuple>
#include <vector>

namespace lotus {
namespace sifa {

template <typename L, typename StateT>
class FixpointLoopSummarizer final : public ILoopSummarizer<L, StateT> {
public:
  using State = StateT;
  using Domain = AbstractDomain<L, State>;
  using RegexRef = lotus::pathexpressions::RegexRef<L>;

  /// \p fixpointTimeout: optional; when set, called each fixpoint iteration; return false to
  /// stop and use domain.top() as summary (Ultimate mFixpointIterationTimeout).
  FixpointLoopSummarizer(SifaStats &stats, const Domain &domain, const IFluid<State> &fluid,
                         DagInterpreter<L, State> &dagIpr,
                         std::function<bool()> fixpointTimeout = nullptr)
      : stats_(stats), domain_(domain), fluid_(fluid), dagIpr_(dagIpr), starDagCache_(stats),
        fixpointTimeout_(std::move(fixpointTimeout)) {}

  State summarize(const lotus::pathexpressions::Star<L> &star, const State &input) override {
    stats_.start(SifaStats::Key::LOOP_SUMMARIZER_OVERALL_TIME);
    stats_.increment(SifaStats::Key::LOOP_SUMMARIZER_APPLICATIONS);

    const void *innerKey = star.getInner() ? star.getInner().get() : nullptr;
    State cached;
    if (lookupCache(innerKey, input, cached)) {
      stats_.stop(SifaStats::Key::LOOP_SUMMARIZER_OVERALL_TIME);
      return cached;
    }

    stats_.increment(SifaStats::Key::LOOP_SUMMARIZER_CACHE_MISSES);
    stats_.start(SifaStats::Key::LOOP_SUMMARIZER_NEW_COMPUTATION_TIME);
    State result = summarizeInternal(star.getInner(), input);
    putCache(innerKey, input, result);
    stats_.stop(SifaStats::Key::LOOP_SUMMARIZER_NEW_COMPUTATION_TIME);
    stats_.stop(SifaStats::Key::LOOP_SUMMARIZER_OVERALL_TIME);
    return result;
  }

private:
  using CacheEntry = std::tuple<const void *, State, State>;
  std::vector<CacheEntry> cache_;

  bool lookupCache(const void *innerKey, const State &input, State &out) const {
    for (const auto &e : cache_) {
      if (std::get<0>(e) == innerKey && domain_.equal(std::get<1>(e), input)) {
        out = std::get<2>(e);
        return true;
      }
    }
    return false;
  }
  void putCache(const void *innerKey, const State &input, const State &result) {
    cache_.emplace_back(innerKey, input, result);
  }

  State summarizeInternal(const RegexRef &inner, const State &input) {
    const auto &dag = starDagCache_.dagOf(inner);
    FullOverlay<L> full;

    State pre = input;
    while (true) {
      if (fixpointTimeout_ && !fixpointTimeout_()) {
        return domain_.top();
      }
      stats_.increment(SifaStats::Key::LOOP_SUMMARIZER_FIXPOINT_ITERATIONS);
      State post = dagIpr_.interpretForSingleMarker(dag, full, pre);
      if (fluid_.shallBeAbstracted(post)) {
        post = domain_.alpha(post);
      }
      auto res = domain_.subsetEq(post, pre);
      post = res.getLhs();
      pre = res.getRhs();
      if (res.isTrueForAbstraction()) {
        break;
      }
      pre = domain_.widen(pre, post);
    }
    return pre;
  }

  SifaStats &stats_;
  const Domain &domain_;
  const IFluid<State> &fluid_;
  DagInterpreter<L, State> &dagIpr_;
  StarDagCache<L> starDagCache_;
  std::function<bool()> fixpointTimeout_;
};

} // namespace sifa
} // namespace lotus

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/SifaSymAbs.h"
extern template class lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, bool>;
extern template class lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, lotus::sifa::SymAbsState>;

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_FIXPOINTLOOPSUMMARIZER_H
