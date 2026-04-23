#pragma once

#include "Solvers/EGraph/Id.h"

#include <vector>

namespace lotus::egraph {

class UnionFind {
public:
  Id makeSet() {
    Id id = Id::fromIndex(parents_.size());
    parents_.push_back(id);
    return id;
  }

  size_t size() const { return parents_.size(); }

  Id find(Id current) const {
    while (current != parents_[current.index()]) {
      current = parents_[current.index()];
    }
    return current;
  }

  Id findMut(Id current) {
    while (current != parents_[current.index()]) {
      Id grandparent = parents_[parents_[current.index()].index()];
      parents_[current.index()] = grandparent;
      current = grandparent;
    }
    return current;
  }

  Id unite(Id root1, Id root2) {
    parents_[root2.index()] = root1;
    return root1;
  }

private:
  std::vector<Id> parents_;
};

} // namespace lotus::egraph
