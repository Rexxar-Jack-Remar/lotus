#ifndef HYBRID_GVFA_ENGINE_H
#define HYBRID_GVFA_ENGINE_H

#include "Analysis/GVFA/GVFAEngine.h"

#include <memory>
#include <mutex>
#include <vector>

namespace gvfa {

class FastGVFAEngine;
class PreciseGVFAEngine;

/**
 * HybridGVFAEngine
 *
 * Always builds a fast reachability index first (cheap pruning), and lazily
 * computes precise reachability/trace info on demand (confirmation + reporting).
 */
class HybridGVFAEngine final : public GVFAEngine {
public:
  HybridGVFAEngine(Module *M, DyckVFG *VFG, DyckAliasAnalysis *DyckAA,
                   DyckModRefAnalysis *DyckMRA,
                   std::vector<std::pair<const Value *, int>> SourcesVec,
                   const VulnerabilitySinksType &Sinks);
  ~HybridGVFAEngine() override;

  void run() override;

  int reachable(const Value *V, int Mask) override;
  bool backwardReachable(const Value *V) override;
  bool srcReachable(const Value *V, const Value *Src) const override;
  bool backwardReachableAllSinks(const Value *V) override;
  std::vector<const Value *> getWitnessPath(const Value *From,
                                            const Value *To) const override;

private:
  void ensurePrecise() const;

  std::vector<std::pair<const Value *, int>> OriginalSourcesVec;

  std::unique_ptr<FastGVFAEngine> Fast;
  mutable std::unique_ptr<PreciseGVFAEngine> Precise;
  mutable std::once_flag PreciseOnce;
};

} // namespace gvfa

#endif
