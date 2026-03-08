#ifndef PRECISE_GVFA_ENGINE_H
#define PRECISE_GVFA_ENGINE_H

#include "GVFAEngine.h"

#include <unordered_map>
#include <unordered_set>

namespace gvfa {

struct SourceSiteHash {
  size_t operator()(const ValueSitePairType &Site) const {
    size_t ValueHash = std::hash<const Value *>()(Site.first);
    size_t SiteHash = std::hash<int>()(Site.second);
    return ValueHash ^ (SiteHash + 0x9e3779b9 + (ValueHash << 6) +
                        (ValueHash >> 2));
  }
};

class PreciseGVFAEngine : public GVFAEngine {
  // All-pairs reachability maps
  std::unordered_map<const Value *,
                     std::unordered_set<ValueSitePairType, SourceSiteHash>>
      AllReachabilityMap;
  std::unordered_map<const Value *, std::unordered_set<const Value *>>
      AllBackwardReachabilityMap;
  std::vector<ValueSitePairType> OriginalSourceSites;
  std::unordered_map<int, int> SourceSiteMasks;

public:
  PreciseGVFAEngine(Module *M, DyckVFG *VFG, DyckAliasAnalysis *DyckAA,
                    DyckModRefAnalysis *DyckMRA,
                    std::vector<std::pair<const Value *, int>> SourcesVec,
                    std::vector<ValueSitePairType> SourceSitesVec,
                    const VulnerabilitySinksType &Sinks);

  void run() override;

  bool srcReachable(const Value *V,
                    const ValueSitePairType &Src) const override;
  bool backwardReachableSink(const Value *V) override;
  bool backwardReachableAllSinks(const Value *V) override;
  std::vector<const Value *> getWitnessPath(const Value *From,
                                            const Value *To) const override;

  // Override to use AllReachabilityMap
  int reachable(const Value *V, int Mask) override;

private:
  void extendSourceSites(std::vector<ValueSitePairType> &SourceSites);
  void detailedForwardRun(const std::vector<ValueSitePairType> &SourceSites);
  void detailedBackwardRun();
  void detailedForwardReachability(const Value *V,
                                   const ValueSitePairType &Src);
  void detailedBackwardReachability(const Value *V, const Value *Sink);

  bool allCount(const Value *V, const ValueSitePairType &Src);
  bool allBackwardCount(const Value *V, const Value *Sink);
};

} // namespace gvfa

#endif
