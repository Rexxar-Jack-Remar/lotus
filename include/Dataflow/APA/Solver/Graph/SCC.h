#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace elimination::detail {

struct SCCDecomposition final {
  struct Component {
    std::vector<std::size_t> Nodes;
  };
  std::vector<Component> Components;
  std::vector<std::size_t> ComponentOf;
};

// Iterative Tarjan traversal: no native recursion on deep equation graphs.
inline SCCDecomposition
computeSCCs(const std::vector<std::vector<std::size_t>> &Succ) {
  SCCDecomposition Out;
  const auto N = Succ.size();
  Out.ComponentOf.resize(N);
  std::vector<std::size_t> Index(N, N), Low(N), Active;
  std::vector<bool> OnStack(N, false);
  struct Frame {
    std::size_t Node, Next;
  };
  std::vector<Frame> DFS;
  std::size_t NextIndex = 0;
  auto Discover = [&](std::size_t V) {
    Index[V] = Low[V] = NextIndex++;
    Active.push_back(V);
    OnStack[V] = true;
    DFS.push_back({V, 0});
  };
  for (std::size_t Root = 0; Root < N; ++Root) {
    if (Index[Root] != N)
      continue;
    Discover(Root);
    while (!DFS.empty()) {
      auto &Top = DFS.back();
      const auto V = Top.Node;
      if (Top.Next < Succ[V].size()) {
        const auto W = Succ[V][Top.Next++];
        if (Index[W] == N)
          Discover(W);
        else if (OnStack[W])
          Low[V] = std::min(Low[V], Index[W]);
        continue;
      }
      if (Low[V] == Index[V]) {
        SCCDecomposition::Component C;
        for (;;) {
          const auto W = Active.back();
          Active.pop_back();
          OnStack[W] = false;
          Out.ComponentOf[W] = Out.Components.size();
          C.Nodes.push_back(W);
          if (W == V)
            break;
        }
        std::sort(C.Nodes.begin(), C.Nodes.end());
        Out.Components.push_back(std::move(C));
      }
      DFS.pop_back();
      if (!DFS.empty())
        Low[DFS.back().Node] = std::min(Low[DFS.back().Node], Low[V]);
    }
  }
  return Out;
}

} // namespace elimination::detail
