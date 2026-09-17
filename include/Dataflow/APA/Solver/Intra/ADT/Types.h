#pragma once

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace elimination::detail {

template <typename NodeT, typename ExprRefT> struct ADTTypes {
  using n_t = NodeT;
  using expr_ref_t = ExprRefT;
  // Normalized edge representation used by the internal reducible-view logic
  // and the ADT construction.
  struct Edge final {
    n_t Src{};
    n_t Dst{};
  };

  struct ADTNode final {
    bool Leaf = false;
    n_t FlowNode{};
    ADTNode *Left = nullptr;
    ADTNode *Right = nullptr;
    ADTNode *Parent = nullptr;
    std::size_t Depth = 0;
    // Entry node of the interval represented by this ADT node. For internal
    // nodes this is the left child's entry, matching the decomposition-tree
    // conventions used in the elimination papers.
    n_t Entry{};
    // Inclusive leaf-position range in reducible topological order.
    int MinPos = 0;
    int MaxPos = 0;
    // Cross edges classified at this composition node.
    std::vector<Edge> F;
    std::vector<Edge> B;
    // Delayed-engine union-find metadata.
    ADTNode *UFParent = nullptr;
    expr_ref_t UFExpr;
    // Simple-engine eagerly propagated expression for the leaf/interval.
    expr_ref_t SimpleExpr;
  };

  // LCA table over the ADT. The F/B-set computation uses this to identify the
  // lowest composition node whose left/right children are crossed by a CFG
  // edge.
  struct LCATable final {
    std::vector<ADTNode *> Euler;
    std::vector<int> Depth;
    std::unordered_map<ADTNode *, int> First;
    std::vector<std::vector<int>> Sparse;
    std::vector<int> Log2;

    void build(ADTNode *Root) {
      Euler.clear();
      Depth.clear();
      First.clear();
      Sparse.clear();
      Log2.clear();
      if (!Root) {
        return;
      }

      struct Frame {
        ADTNode *Node = nullptr;
        int Depth = 0;
        int Stage = 0;
      };

      std::vector<Frame> Stack;
      Stack.push_back({Root, 0, 0});
      while (!Stack.empty()) {
        auto Frame = Stack.back();
        Stack.pop_back();
        if (!Frame.Node) {
          continue;
        }
        if (Frame.Stage == 0) {
          if (!First.count(Frame.Node)) {
            First.emplace(Frame.Node, static_cast<int>(Euler.size()));
          }
          Euler.push_back(Frame.Node);
          Depth.push_back(Frame.Depth);

          if (Frame.Node->Right) {
            Stack.push_back({Frame.Node, Frame.Depth, 2});
            Stack.push_back({Frame.Node->Right, Frame.Depth + 1, 0});
          }
          if (Frame.Node->Left) {
            Stack.push_back({Frame.Node, Frame.Depth, 1});
            Stack.push_back({Frame.Node->Left, Frame.Depth + 1, 0});
          }
          continue;
        }

        Euler.push_back(Frame.Node);
        Depth.push_back(Frame.Depth);
      }

      const int M = static_cast<int>(Euler.size());
      if (M == 0) {
        return;
      }
      Log2.resize(M + 1);
      Log2[1] = 0;
      for (int i = 2; i <= M; ++i) {
        Log2[i] = Log2[i / 2] + 1;
      }

      const int K = Log2[M];
      Sparse.assign(K + 1, std::vector<int>(M));
      for (int i = 0; i < M; ++i) {
        Sparse[0][i] = i;
      }
      for (int k = 1; k <= K; ++k) {
        const int Len = 1 << k;
        const int Half = Len >> 1;
        for (int i = 0; i + Len <= M; ++i) {
          const int I1 = Sparse[k - 1][i];
          const int I2 = Sparse[k - 1][i + Half];
          Sparse[k][i] = (Depth[I1] <= Depth[I2]) ? I1 : I2;
        }
      }
    }

    ADTNode *query(ADTNode *A, ADTNode *B) const {
      if (!A || !B) {
        return nullptr;
      }
      auto ItA = First.find(A);
      auto ItB = First.find(B);
      if (ItA == First.end() || ItB == First.end()) {
        return nullptr;
      }
      int L = ItA->second;
      int R = ItB->second;
      if (L > R) {
        std::swap(L, R);
      }
      const int Len = R - L + 1;
      const int K = Log2[Len];
      const int I1 = Sparse[K][L];
      const int I2 = Sparse[K][R - (1 << K) + 1];
      return (Depth[I1] <= Depth[I2]) ? Euler[I1] : Euler[I2];
    }
  };
};

} // namespace elimination::detail
