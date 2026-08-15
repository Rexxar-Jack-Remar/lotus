#pragma once

#include "Solvers/EGraph/Id.h"
#include "Solvers/EGraph/Language.h"

namespace lotus::egraph {

template <typename L, typename D> struct EClass {
  using MatchingNodes =
      std::unordered_map<typename L::Discriminant, std::vector<size_t>>;
  static constexpr size_t MATCHING_INDEX_THRESHOLD = 16;

  Id id;
  std::vector<L> nodes;
  D data;
  std::vector<Id> parents;
  std::shared_ptr<MatchingNodes> matching_nodes;
  bool nodes_dirty = true;

  bool empty() const { return nodes.empty(); }
  size_t size() const { return nodes.size(); }

  auto begin() const { return nodes.begin(); }
  auto end() const { return nodes.end(); }

  auto iter() const { return nodes.begin(); }

  const std::vector<Id> &parentIds() const { return parents; }
  bool isEmpty() const { return empty(); }
  size_t len() const { return size(); }

  auto parentsIter() const { return parents.begin(); }

  void rebuildMatchingIndex() {
    if (nodes.size() < MATCHING_INDEX_THRESHOLD) {
      matching_nodes.reset();
      return;
    }

    auto index = matching_nodes && matching_nodes.use_count() == 1
                     ? std::move(matching_nodes)
                     : std::make_shared<MatchingNodes>();
    index->clear();
    index->reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
      (*index)[nodes[i].discriminant()].push_back(i);
    }
    matching_nodes = std::move(index);
  }

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
  if (klass.matching_nodes) {
    auto indexed = klass.matching_nodes->find(node.discriminant());
    if (indexed == klass.matching_nodes->end()) {
      return true;
    }
    for (size_t index : indexed->second) {
      const auto &candidate = klass.nodes[index];
      if (node.matches(candidate) && !fn(candidate)) {
        return false;
      }
    }
    return true;
  }

  for (const auto &candidate : klass.nodes) {
    if (node.matches(candidate) && !fn(candidate)) {
      return false;
    }
  }
  return true;
}

} // namespace lotus::egraph
