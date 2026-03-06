#ifndef ANALYSIS_GVFA_ENGINE_H
#define ANALYSIS_GVFA_ENGINE_H

#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"

#include <queue>
#include <set>
#include <unordered_map>
#include <vector>

using namespace llvm;

namespace gvfa {

class GVFAEngine {
protected:
  Module *M;
  DyckVFG *VFG;
  DyckAliasAnalysis *DyckAA;
  DyckModRefAnalysis *DyckMRA;

  std::vector<std::pair<const Value *, int>> SourcesVec;
  const VulnerabilitySinksType &Sinks;

  // Reachability Maps
  // ReachabilityMap is used for:
  // 1. Source extension (visited set) in BOTH modes
  // 2. Propagation in Fast (BitVector) mode
  std::unordered_map<const Value *, int> ReachabilityMap;

  // Backward Reachability Map for Fast mode
  std::unordered_map<const Value *, int> BackwardReachabilityMap;

public:
  GVFAEngine(Module *M, DyckVFG *VFG, DyckAliasAnalysis *DyckAA,
             DyckModRefAnalysis *DyckMRA,
             std::vector<std::pair<const Value *, int>> SourcesVec,
             const VulnerabilitySinksType &Sinks);

  virtual ~GVFAEngine() = default;

  virtual void run() = 0;

  // Query Interface
  virtual int reachable(const Value *V, int Mask);
  virtual bool backwardReachable(const Value *V);
  virtual bool srcReachable(const Value *V, const Value *Src) const = 0;
  virtual bool backwardReachableSink(const Value *V);
  virtual bool backwardReachableAllSinks(const Value *V);
  virtual std::vector<const Value *> getWitnessPath(const Value *From,
                                                    const Value *To) const = 0;

  // Stats
  size_t getForwardMapSize() const { return ReachabilityMap.size(); }
  size_t getBackwardMapSize() const { return BackwardReachabilityMap.size(); }

protected:
  // Common algorithms
  void extendSources(std::vector<std::pair<const Value *, int>> &Sources);

  // Helpers for BitVector logic
  int count(const Value *V, int Mask);
  bool count(const Value *V);
  int backwardCount(const Value *V);

  // Worklist helpers
  void processCallSite(const CallInst *CI, const Value *V, int Mask,
                       std::queue<std::pair<const Value *, int>> &WorkQueue);
  void processReturnSite(const ReturnInst *RI, const Value *V, int Mask,
                         std::queue<std::pair<const Value *, int>> &WorkQueue);
};

} // namespace gvfa

#endif
