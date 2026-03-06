#ifndef PRECISE_GVFA_ENGINE_H
#define PRECISE_GVFA_ENGINE_H

#include "GVFAEngine.h"

#include <unordered_set>

namespace gvfa {

class PreciseGVFAEngine : public GVFAEngine {
  // All-pairs reachability maps
  std::unordered_map<const Value *, std::unordered_set<const Value *>>
      AllReachabilityMap;
  std::unordered_map<const Value *, std::unordered_set<const Value *>>
      AllBackwardReachabilityMap;

public:
  using GVFAEngine::GVFAEngine;

  void run() override;

  bool srcReachable(const Value *V, const Value *Src) const override;
  bool backwardReachableSink(const Value *V) override;
  bool backwardReachableAllSinks(const Value *V) override;
  std::vector<const Value *> getWitnessPath(const Value *From,
                                            const Value *To) const override;

  // Override to use AllReachabilityMap
  int reachable(const Value *V, int Mask) override;

private:
  void detailedForwardRun();
  void detailedBackwardRun();
  void detailedForwardReachability(const Value *V, const Value *Src);
  void detailedBackwardReachability(const Value *V, const Value *Sink);

  bool allCount(const Value *V, const Value *Src);
  bool allBackwardCount(const Value *V, const Value *Sink);
};

} // namespace gvfa

#endif
