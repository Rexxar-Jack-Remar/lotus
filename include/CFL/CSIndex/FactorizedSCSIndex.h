#pragma once

#include "CFL/CSIndex/SCSIndex.h"

#include <utility>
#include <vector>

/**
 * Exact OR-composition for independently updated category policies whose joint
 * vulnerability condition is disjunctive.
 */
class FactorizedSCSIndex {
public:
  explicit FactorizedSCSIndex(std::vector<SCSIndex *> indexes)
      : indexes_(std::move(indexes)) {}

  bool reachable(int source, int sink) {
    for (SCSIndex *index : indexes_) {
      if (index && index->reachable(source, sink))
        return true;
    }
    return false;
  }

private:
  std::vector<SCSIndex *> indexes_;
};
