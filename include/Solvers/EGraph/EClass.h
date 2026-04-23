#pragma once

#include "Solvers/EGraph/Id.h"
#include "Solvers/EGraph/Language.h"

namespace lotus::egraph {

template <typename L, typename D> struct EClass {
  Id id;
  std::vector<L> nodes;
  D data;
  std::vector<Id> parents;

  bool empty() const { return nodes.empty(); }
  size_t size() const { return nodes.size(); }
};

} // namespace lotus::egraph
