/*
HybridGVFAEngine.cpp

Hybrid engine:
- Run FastGVFAEngine once to build a cheap reachability index (pruning).
- Lazily build/run PreciseGVFAEngine when a query needs attribution or a good
trace.
*/

#include "Analysis/GVFA/HybridGVFAEngine.h"

#include "Analysis/GVFA/FastGVFAEngine.h"
#include "Analysis/GVFA/PreciseGVFAEngine.h"

using namespace llvm;
using namespace gvfa;

HybridGVFAEngine::HybridGVFAEngine(
    Module *M, DyckVFG *VFG, DyckAliasAnalysis *DyckAA,
    DyckModRefAnalysis *DyckMRA,
    std::vector<std::pair<const Value *, int>> SourcesVec,
    const VulnerabilitySinksType &Sinks)
    : GVFAEngine(M, VFG, DyckAA, DyckMRA, {}, Sinks),
      OriginalSourcesVec(std::move(SourcesVec)) {}

HybridGVFAEngine::~HybridGVFAEngine() = default;

void HybridGVFAEngine::run() {
  Fast = std::make_unique<FastGVFAEngine>(M, VFG, DyckAA, DyckMRA,
                                          OriginalSourcesVec, Sinks);
  Fast->run();
}

int HybridGVFAEngine::reachable(const Value *V, int Mask) {
  if (!Fast) {
    return 0;
  }
  return Fast->reachable(V, Mask);
}

bool HybridGVFAEngine::backwardReachable(const Value *V) {
  if (!Fast) {
    return false;
  }
  return Fast->backwardReachable(V);
}

void HybridGVFAEngine::ensurePrecise() const {
  std::call_once(PreciseOnce, [this]() {
    auto engine = std::make_unique<PreciseGVFAEngine>(
        M, VFG, DyckAA, DyckMRA, OriginalSourcesVec, Sinks);
    engine->run();
    Precise = std::move(engine);
  });
}

bool HybridGVFAEngine::srcReachable(const Value *V, const Value *Src) const {
  ensurePrecise();
  return Precise ? Precise->srcReachable(V, Src) : false;
}

bool HybridGVFAEngine::backwardReachableAllSinks(const Value *V) {
  ensurePrecise();
  return Precise ? Precise->backwardReachableAllSinks(V) : false;
}

std::vector<const Value *>
HybridGVFAEngine::getWitnessPath(const Value *From, const Value *To) const {
  ensurePrecise();
  return Precise ? Precise->getWitnessPath(From, To)
                 : (Fast ? Fast->getWitnessPath(From, To)
                         : std::vector<const Value *>{});
}
