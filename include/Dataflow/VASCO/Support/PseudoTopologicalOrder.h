#pragma once

#include "Dataflow/VASCO/Core/DirectedGraph.h"

#include <algorithm>
#include <set>
#include <vector>

namespace vasco {

template <typename N>
static void dfsPseudoTopological(const DirectedGraph<N> &Graph, const N &Node,
                                 bool Reverse, std::set<N> &Visited,
                                 std::vector<N> &PostOrder) {
  if (!Visited.insert(Node).second) {
    return;
  }

  const auto NextNodes =
      Reverse ? Graph.predsOf(Node) : Graph.succsOf(Node);
  for (const auto &Next : NextNodes) {
    dfsPseudoTopological(Graph, Next, Reverse, Visited, PostOrder);
  }

  PostOrder.push_back(Node);
}

template <typename N>
std::vector<N> computePseudoTopologicalOrder(const DirectedGraph<N> &Graph,
                                             bool Reverse) {
  std::set<N> Visited;
  std::vector<N> PostOrder;

  const auto Seeds = Reverse ? Graph.tails() : Graph.heads();
  for (const auto &Seed : Seeds) {
    dfsPseudoTopological(Graph, Seed, Reverse, Visited, PostOrder);
  }

  for (const auto &Node : Graph.nodes()) {
    dfsPseudoTopological(Graph, Node, Reverse, Visited, PostOrder);
  }

  std::reverse(PostOrder.begin(), PostOrder.end());
  return PostOrder;
}

} // namespace vasco
