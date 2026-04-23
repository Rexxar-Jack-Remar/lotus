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

  auto begin() const { return nodes.begin(); }
  auto end() const { return nodes.end(); }

  auto iter() const { return nodes.begin(); }

  std::vector<Id> parentIds() const { return parents; }
  bool isEmpty() const { return empty(); }
  size_t len() const { return size(); }

  auto parentsIter() const { return parents.begin(); }

  std::vector<std::reference_wrapper<const L>> leaves() const {
    std::vector<std::reference_wrapper<const L>> out;
    for (const auto &node : nodes) {
      if (node.children().empty()) {
        out.emplace_back(node);
      }
    }
    return out;
  }

  void assertUniqueLeaves() const {
    const L *first = nullptr;
    for (const auto &node : nodes) {
      if (!node.children().empty()) {
        continue;
      }
      if (!first) {
        first = &node;
        continue;
      }
      if (!(node == *first)) {
        throw std::runtime_error("Different leaves in eclass");
      }
    }
  }
};

template <typename L, typename D, typename F>
inline bool forEachMatchingNode(const EClass<L, D> &klass, const L &node,
                                F &&fn) {
  if (klass.nodes.size() < 50) {
    for (const auto &candidate : klass.nodes) {
      if (node.matches(candidate) && !fn(candidate)) {
        return false;
      }
    }
    return true;
  }

  auto discr = node.discriminant();
  auto begin = std::lower_bound(
      klass.nodes.begin(), klass.nodes.end(), node,
      [](const L &lhs, const L &rhs) { return lhs < rhs; });
  while (begin != klass.nodes.begin() &&
         (begin - 1)->discriminant() == discr) {
    --begin;
  }

  for (auto it = begin; it != klass.nodes.end() && it->discriminant() == discr;
       ++it) {
    if (node.matches(*it) && !fn(*it)) {
      return false;
    }
  }
  return true;
}

} // namespace lotus::egraph
