#pragma once
#include "IR/GVFG/GuardedValueFlowObject.h"

#include <functional>
namespace lotus {
namespace gsaf {
/// Orders graph-owned trace references without copying their IR objects.
struct ObjectLess {
  bool operator()(const gvfg::GuardedValueFlowObject *left,
                  const gvfg::GuardedValueFlowObject *right) const {
    if (left == right)
      return false;
    if (!left || !right)
      return std::less<const gvfg::GuardedValueFlowObject *>()(left, right);
    if (left->getGraph() != right->getGraph())
      return std::less<gvfg::GuardedValueFlowGraph *>()(left->getGraph(),
                                                        right->getGraph());
    if (left->getObjectId() != right->getObjectId())
      return left->getObjectId() < right->getObjectId();
    return std::less<const gvfg::GuardedValueFlowObject *>()(left, right);
  }
};

} // namespace gsaf
} // namespace lotus
