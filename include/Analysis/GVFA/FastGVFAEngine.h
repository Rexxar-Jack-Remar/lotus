#ifndef FAST_GVFA_ENGINE_H
#define FAST_GVFA_ENGINE_H

#include "GVFAEngine.h"

namespace gvfa {

class FastGVFAEngine : public GVFAEngine {
public:
  using GVFAEngine::GVFAEngine;

  void run() override;
  bool srcReachable(const Value *V, const Value *Src) const override;
  std::vector<const Value *> getWitnessPath(const Value *From,
                                            const Value *To) const override;

private:
  void forwardRun();
  void backwardRun();
  void forwardReachability(const Value *V, int Mask);
  void backwardReachability(const Value *V);
};

} // namespace gvfa

#endif
