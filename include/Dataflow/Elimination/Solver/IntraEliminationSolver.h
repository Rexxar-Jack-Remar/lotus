#ifndef DATAFLOW_ELIMINATION_SOLVER_INTRAELIMINATIONSOLVER_H_
#define DATAFLOW_ELIMINATION_SOLVER_INTRAELIMINATIONSOLVER_H_

#include "Dataflow/Elimination/DataFlowResult.h"
#include "Dataflow/Elimination/EliminationFramework.h"
#include "Dataflow/Elimination/Options.h"
#include "Dataflow/Elimination/PathExpression.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace elimination {

template <typename AnalysisDomainTy>
class IntraEliminationSolver final {
public:
  using ProblemTy = IntraEliminationProblem<AnalysisDomainTy>;
  using ReducibleProblemTy = IntraReducibleEliminationProblem<AnalysisDomainTy>;
  using n_t = typename ProblemTy::n_t;
  using fact_t = typename ProblemTy::fact_t;
  using transfer_t = typename ProblemTy::transfer_t;

  using expr_factory_t = PathExprFactory<transfer_t>;
  using expr_ref_t = typename expr_factory_t::Ref;
  using result_t = DataFlowResultT<n_t, fact_t, transfer_t>;

  explicit IntraEliminationSolver(const ProblemTy &Problem,
                                 EliminationOptions Opts = {})
      : Problem(Problem), Opts(Opts) {}

  void solve() {
    if (Opts.Method == EliminationMethod::ADTSimple) {
      if (trySolveADTSimple()) {
        UsedADT = true;
        return;
      }
    }
    if (Opts.Method == EliminationMethod::ADTDelayed) {
      if (trySolveADTDelayed()) {
        UsedADT = true;
        return;
      }
    }
    solveStateElimination();
  }

  const result_t &getResults() const { return Results; }
  bool usedADT() const { return UsedADT; }

private:
  void solveStateElimination() {
    buildMatrix();
    eliminateAllIntermediates();
    materializeResultsFromMatrix();
  }

  void buildMatrix() {
    Nodes = Problem.nodes();
    Index.clear();
    Index.reserve(Nodes.size());
    for (std::size_t i = 0; i < Nodes.size(); ++i) {
      Index.emplace(Nodes[i], i);
    }

    const auto N = Nodes.size();
    Matrix.assign(N, std::vector<expr_ref_t>(N, Exprs.zero()));
    for (std::size_t i = 0; i < N; ++i) {
      Matrix[i][i] = Exprs.one();
    }

    for (const auto &Src : Nodes) {
      const auto SrcIdx = idx(Src);
      for (const auto &Dst : Problem.succs(Src)) {
        const auto It = Index.find(Dst);
        if (It == Index.end()) {
          continue;
        }
        const auto DstIdx = It->second;
        Matrix[SrcIdx][DstIdx] =
            Exprs.unite(Matrix[SrcIdx][DstIdx],
                        Exprs.atom(Problem.edgeTransfer(Src, Dst)));
      }
    }
  }

  void eliminateAllIntermediates() {
    const auto N = Nodes.size();
    std::vector<expr_ref_t> ColK;
    std::vector<expr_ref_t> RowK;
    ColK.resize(N);
    RowK.resize(N);

    for (std::size_t k = 0; k < N; ++k) {
      for (std::size_t i = 0; i < N; ++i) {
        ColK[i] = Matrix[i][k];
      }
      for (std::size_t j = 0; j < N; ++j) {
        RowK[j] = Matrix[k][j];
      }

      const auto KStar = Exprs.star(Matrix[k][k]);
      for (std::size_t i = 0; i < N; ++i) {
        if (expr_factory_t::isZero(ColK[i])) {
          continue;
        }
        for (std::size_t j = 0; j < N; ++j) {
          if (expr_factory_t::isZero(RowK[j])) {
            continue;
          }
          auto Via = Exprs.concat(ColK[i], KStar);
          Via = Exprs.concat(Via, RowK[j]);
          Matrix[i][j] = Exprs.unite(Matrix[i][j], Via);
        }
      }
    }
  }

  void materializeResultsFromMatrix() {
    Results = result_t{};
    if (Nodes.empty()) {
      return;
    }

    const auto EntryIt = Index.find(Problem.entry());
    assert(EntryIt != Index.end() &&
           "Problem.entry() must be included in Problem.nodes()");
    const auto EntryIdx = EntryIt->second;

    const auto Init = Problem.initialFact();
    for (std::size_t j = 0; j < Nodes.size(); ++j) {
      const auto &N = Nodes[j];
      auto E = Matrix[EntryIdx][j];
      Results.ExprTo(N) = E;
      Results.IN(N) = eval(E, Init);
    }
  }

  // --- ADT (paper-style) ---
  struct Edge final {
    n_t Src{};
    n_t Dst{};
  };

  struct ADTNode final {
    bool Leaf = false;
    n_t FlowNode{};
    ADTNode *Left = nullptr;
    ADTNode *Right = nullptr;
    ADTNode *Parent = nullptr; // structural parent in the ADT
    std::size_t Depth = 0;

    // Entry of this interval (paper: r): for leaves, it's itself; for internal
    // nodes, it's the entry of the left child interval.
    n_t Entry{};

    // Leaf-order range (by reducible topological order).
    int MinPos = 0;
    int MaxPos = 0;

    // F/B sets for composition nodes (paper Fig. 5).
    std::vector<Edge> F;
    std::vector<Edge> B;

    // Union-find link for delayed evaluation (paper Fig. 6).
    ADTNode *UFParent = nullptr;
    expr_ref_t UFExpr; // expression from UFParent's entry to this node's entry

    // Simple algorithm: path expression from this interval entry to FlowNode.
    expr_ref_t SimpleExpr;
  };

  static bool containsPos(const ADTNode *N, int Pos) {
    return N && N->MinPos <= Pos && Pos <= N->MaxPos;
  }

  static ADTNode *childContaining(ADTNode *W, int Pos) {
    if (!W || W->Leaf) {
      return nullptr;
    }
    if (containsPos(W->Left, Pos)) {
      return W->Left;
    }
    if (containsPos(W->Right, Pos)) {
      return W->Right;
    }
    return nullptr;
  }

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
        int Stage = 0; // 0 = enter, 1 = after left, 2 = after right
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

        // After visiting a child, record this node again.
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

  void computeEntriesAndRanges(
      ADTNode *N, const std::unordered_map<n_t, int> &TopoPos) {
    if (!N) {
      return;
    }
    if (N->Leaf) {
      N->Entry = N->FlowNode;
      const auto It = TopoPos.find(N->FlowNode);
      assert(It != TopoPos.end());
      N->MinPos = It->second;
      N->MaxPos = It->second;
      return;
    }
    computeEntriesAndRanges(N->Left, TopoPos);
    computeEntriesAndRanges(N->Right, TopoPos);
    assert(N->Left && N->Right);
    N->Entry = N->Left->Entry;
    N->MinPos = std::min(N->Left->MinPos, N->Right->MinPos);
    N->MaxPos = std::max(N->Left->MaxPos, N->Right->MaxPos);
  }

  void initUF(ADTNode *N) {
    if (!N) {
      return;
    }
    N->UFParent = N;
    N->UFExpr = Exprs.one();
    if (N->Left) {
      initUF(N->Left);
    }
    if (N->Right) {
      initUF(N->Right);
    }
  }

  void linkUpdate(ADTNode *Parent, ADTNode *Child, const expr_ref_t &Prefix) {
    assert(Parent && Child);
    Child->UFParent = Parent;
    Child->UFExpr = Prefix;
  }

  expr_ref_t evalUF(ADTNode *X) {
    assert(X);
    if (X->UFParent == X) {
      return X->UFExpr;
    }
    auto *P = X->UFParent;
    assert(P);
    if (P->UFParent != P) {
      X->UFExpr = Exprs.concat(evalUF(P), X->UFExpr);
      X->UFParent = P->UFParent;
    }
    return X->UFExpr;
  }

  struct ReducibleViewProvided final {
    const ReducibleProblemTy *R = nullptr;
    std::vector<Edge> Edges;
    std::vector<n_t> Topo;

    explicit ReducibleViewProvided(const ReducibleProblemTy &R) : R(&R) {}

    bool init() {
      Topo = R->topologicalOrder();
      if (Topo.empty()) {
        return false;
      }
      Edges.clear();
      const auto REdges = R->edges();
      Edges.reserve(REdges.size());
      for (const auto &E : REdges) {
        Edges.push_back({E.Src, E.Dst});
      }
      return true;
    }

    const std::vector<Edge> &edges() const { return Edges; }
    const std::vector<n_t> &topologicalOrder() const { return Topo; }
    n_t idom(n_t N) const { return R->idom(N); }
    bool dominates(n_t A, n_t B) const { return R->dominates(A, B); }
    bool isBackEdge(n_t Src, n_t Dst) const {
      return R->isBackEdge(Src, Dst);
    }
    transfer_t edgeTransfer(n_t Src, n_t Dst) const {
      return R->edgeTransfer(Src, Dst);
    }
  };

  struct ComputedReducibleView final {
    const ProblemTy *Problem = nullptr;
    std::vector<n_t> Nodes;
    std::vector<Edge> Edges;
    std::vector<n_t> Topo;
    std::unordered_map<n_t, std::size_t> NodeIndex;
    std::vector<std::vector<std::size_t>> Preds;
    std::vector<std::vector<uint64_t>> DomBits;
    std::vector<n_t> Idom;

    const std::vector<Edge> &edges() const { return Edges; }
    const std::vector<n_t> &topologicalOrder() const { return Topo; }
    n_t idom(n_t N) const { return Idom.at(NodeIndex.at(N)); }
    bool dominates(n_t A, n_t B) const {
      const auto &Bits = DomBits[NodeIndex.at(B)];
      const auto Idx = NodeIndex.at(A);
      return (Bits[Idx / 64] >> (Idx % 64)) & 1U;
    }
    bool isBackEdge(n_t Src, n_t Dst) const {
      return dominates(Dst, Src);
    }
    transfer_t edgeTransfer(n_t Src, n_t Dst) const {
      return Problem->edgeTransfer(Src, Dst);
    }
  };

  static std::vector<uint64_t> bitsetAllOnes(std::size_t Bits) {
    const std::size_t Words = (Bits + 63) / 64;
    std::vector<uint64_t> Out(Words, ~uint64_t(0));
    if (Bits % 64) {
      Out.back() &= ((uint64_t(1) << (Bits % 64)) - 1);
    }
    return Out;
  }

  static std::vector<uint64_t> bitsetZero(std::size_t Bits) {
    return std::vector<uint64_t>((Bits + 63) / 64, uint64_t(0));
  }

  static void bitsetSet(std::vector<uint64_t> &Bits, std::size_t Idx) {
    Bits[Idx / 64] |= (uint64_t(1) << (Idx % 64));
  }

  static bool bitsetTest(const std::vector<uint64_t> &Bits,
                                       std::size_t Idx) {
    return (Bits[Idx / 64] >> (Idx % 64)) & 1U;
  }

  static void bitsetAndInplace(std::vector<uint64_t> &Dst,
                               const std::vector<uint64_t> &Src) {
    for (std::size_t i = 0; i < Dst.size(); ++i) {
      Dst[i] &= Src[i];
    }
  }

  static bool bitsetEqual(const std::vector<uint64_t> &A,
                                        const std::vector<uint64_t> &B) {
    if (A.size() != B.size()) {
      return false;
    }
    for (std::size_t i = 0; i < A.size(); ++i) {
      if (A[i] != B[i]) {
        return false;
      }
    }
    return true;
  }

  bool buildComputedReducibleView(ComputedReducibleView &Out) {
    Out = ComputedReducibleView{};
    Out.Problem = &Problem;
    Out.Nodes = Problem.nodes();
    if (Out.Nodes.empty()) {
      return false;
    }

    Out.NodeIndex.clear();
    Out.NodeIndex.reserve(Out.Nodes.size());
    for (std::size_t i = 0; i < Out.Nodes.size(); ++i) {
      Out.NodeIndex.emplace(Out.Nodes[i], i);
    }

    const auto Entry = Problem.entry();
    const auto EntryIt = Out.NodeIndex.find(Entry);
    if (EntryIt == Out.NodeIndex.end()) {
      return false;
    }
    const std::size_t EntryIdx = EntryIt->second;

    // Reachability check.
    std::unordered_set<n_t> Reach;
    Reach.reserve(Out.Nodes.size());
    std::vector<n_t> Stack;
    Stack.push_back(Entry);
    Reach.insert(Entry);
    while (!Stack.empty()) {
      const auto Cur = Stack.back();
      Stack.pop_back();
      for (const auto &Succ : Problem.succs(Cur)) {
        if (Out.NodeIndex.find(Succ) == Out.NodeIndex.end()) {
          continue;
        }
        if (Reach.insert(Succ).second) {
          Stack.push_back(Succ);
        }
      }
    }
    if (Reach.size() != Out.Nodes.size()) {
      return false;
    }

    // Build edge list and predecessor lists.
    Out.Edges.clear();
    Out.Preds.assign(Out.Nodes.size(), {});
    for (const auto &Src : Out.Nodes) {
      const auto SrcIdx = Out.NodeIndex.at(Src);
      for (const auto &Dst : Problem.succs(Src)) {
        auto It = Out.NodeIndex.find(Dst);
        if (It == Out.NodeIndex.end()) {
          continue;
        }
        const auto DstIdx = It->second;
        Out.Edges.push_back({Src, Dst});
        Out.Preds[DstIdx].push_back(SrcIdx);
      }
    }

    // Dominator computation (iterative).
    const std::size_t N = Out.Nodes.size();
    const auto All = bitsetAllOnes(N);
    Out.DomBits.assign(N, bitsetZero(N));
    for (std::size_t i = 0; i < N; ++i) {
      if (i == EntryIdx) {
        Out.DomBits[i] = bitsetZero(N);
        bitsetSet(Out.DomBits[i], i);
      } else {
        Out.DomBits[i] = All;
      }
    }

    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (std::size_t V = 0; V < N; ++V) {
        if (V == EntryIdx) {
          continue;
        }
        if (Out.Preds[V].empty()) {
          return false;
        }
        auto NewBits = All;
        for (const auto P : Out.Preds[V]) {
          bitsetAndInplace(NewBits, Out.DomBits[P]);
        }
        bitsetSet(NewBits, V);
        if (!bitsetEqual(NewBits, Out.DomBits[V])) {
          Out.DomBits[V] = std::move(NewBits);
          Changed = true;
        }
      }
    }

    for (std::size_t V = 0; V < N; ++V) {
      if (!bitsetTest(Out.DomBits[V], EntryIdx)) {
        return false;
      }
    }

    // Immediate dominators: pick the strict dominator of V that does not
    // dominate any other strict dominator of V (i.e., closest to V).
    Out.Idom.assign(N, Out.Nodes.front());
    Out.Idom[EntryIdx] = Out.Nodes[EntryIdx];
    for (std::size_t V = 0; V < N; ++V) {
      if (V == EntryIdx) {
        continue;
      }
      n_t Candidate = Out.Nodes.front();
      bool Found = false;
      for (std::size_t D = 0; D < N; ++D) {
        if (D == V || !bitsetTest(Out.DomBits[V], D)) {
          continue;
        }
        bool IsImmediate = true;
        for (std::size_t D2 = 0; D2 < N; ++D2) {
          if (D2 == V || D2 == D || !bitsetTest(Out.DomBits[V], D2)) {
            continue;
          }
          // If D dominates another strict dominator, it's not immediate.
          if (bitsetTest(Out.DomBits[D2], D)) {
            IsImmediate = false;
            break;
          }
        }
        if (IsImmediate) {
          Candidate = Out.Nodes[D];
          Found = true;
          break;
        }
      }
      if (!Found) {
        return false;
      }
      Out.Idom[V] = Candidate;
    }

    // Topological order of forward edges (non-back edges).
    std::vector<std::vector<std::size_t>> SuccF(N);
    std::vector<std::size_t> InDeg(N, 0);
    for (const auto &E : Out.Edges) {
      const auto SrcIdx = Out.NodeIndex.at(E.Src);
      const auto DstIdx = Out.NodeIndex.at(E.Dst);
      if (Out.dominates(E.Dst, E.Src)) {
        continue;
      }
      SuccF[SrcIdx].push_back(DstIdx);
      ++InDeg[DstIdx];
    }

    if (InDeg[EntryIdx] != 0) {
      return false;
    }

    std::deque<std::size_t> Ready;
    Ready.push_back(EntryIdx);
    for (std::size_t i = 0; i < N; ++i) {
      if (i == EntryIdx) {
        continue;
      }
      if (InDeg[i] == 0) {
        Ready.push_back(i);
      }
    }

    Out.Topo.clear();
    Out.Topo.reserve(N);
    while (!Ready.empty()) {
      const auto Cur = Ready.front();
      Ready.pop_front();
      Out.Topo.push_back(Out.Nodes[Cur]);
      for (const auto S : SuccF[Cur]) {
        if (--InDeg[S] == 0) {
          Ready.push_back(S);
        }
      }
    }

    if (Out.Topo.size() != N) {
      return false;
    }
    if (Out.Topo.front() != Entry) {
      return false;
    }
    return true;
  }

  template <typename ReducibleViewT>
  bool prepareADT(const ReducibleViewT &R, ADTNode *&Root,
                  std::unordered_map<n_t, ADTNode *> &LeafOf,
                  std::unordered_map<n_t, int> &TopoPos,
                  std::vector<ADTNode *> &LeafByPos, LCATable &Lca) {
    const auto &Topo = R.topologicalOrder();
    if (Topo.empty()) {
      return false;
    }

    TopoPos.clear();
    TopoPos.reserve(Topo.size());
    for (int i = 0; i < static_cast<int>(Topo.size()); ++i) {
      TopoPos.emplace(Topo[i], i);
    }

    if (Topo.front() != Problem.entry()) {
      return false;
    }

    for (const auto &N : Problem.nodes()) {
      if (TopoPos.find(N) == TopoPos.end()) {
        return false;
      }
    }

    // Build stacks s_u (paper Fig. 4).
    std::unordered_map<n_t, std::vector<n_t>> Stacks;
    Stacks.reserve(Topo.size());
    for (const auto &N : Topo) {
      (void)Stacks[N];
    }

    for (int i = static_cast<int>(Topo.size()) - 1; i >= 1; --i) {
      const auto U = Topo[i];
      const auto V = R.idom(U);
      auto It = Stacks.find(V);
      if (It == Stacks.end()) {
        return false;
      }
      It->second.push_back(U);
    }

    // Allocate ADT nodes.
    ADTNodes.clear();
    ADTNodes.reserve(2 * Topo.size());
    LeafOf.clear();
    LeafOf.reserve(Topo.size());

    Root = traverseADT(Problem.entry(), Stacks, LeafOf);
    if (!Root) {
      return false;
    }
    Root->Parent = nullptr;
    computeEntriesAndRanges(Root, TopoPos);

    // Leaf-by-position lookup.
    LeafByPos.assign(Topo.size(), nullptr);
    for (const auto &It : LeafOf) {
      const auto PosIt = TopoPos.find(It.first);
      if (PosIt == TopoPos.end()) {
        return false;
      }
      LeafByPos[PosIt->second] = It.second;
    }
    for (auto *Leaf : LeafByPos) {
      if (!Leaf) {
        return false;
      }
    }

    Lca.build(Root);

    // Compute F/B sets (paper Fig. 5) via NCA/LCA in ADT.
    if (!computeFBSets(R, Root, LeafOf, TopoPos, Lca)) {
      return false;
    }

    return true;
  }

  template <typename ReducibleViewT>
  bool solveADTSimpleWith(const ReducibleViewT &R) {
    ADTNode *Root = nullptr;
    std::unordered_map<n_t, ADTNode *> LeafOf;
    std::unordered_map<n_t, int> TopoPos;
    std::vector<ADTNode *> LeafByPos;
    LCATable Lca;
    if (!prepareADT(R, Root, LeafOf, TopoPos, LeafByPos, Lca)) {
      return false;
    }

    if (!computePathExprSimple(R, Root, LeafOf, LeafByPos)) {
      return false;
    }

    Results = result_t{};
    const auto Init = Problem.initialFact();
    for (const auto &N : Problem.nodes()) {
      auto It = LeafOf.find(N);
      if (It == LeafOf.end()) {
        continue;
      }
      auto *Leaf = It->second;
      Results.ExprTo(N) = Leaf->SimpleExpr;
      Results.IN(N) = eval(Leaf->SimpleExpr, Init);
    }
    return true;
  }

  template <typename ReducibleViewT>
  bool solveADTDelayedWith(const ReducibleViewT &R) {
    ADTNode *Root = nullptr;
    std::unordered_map<n_t, ADTNode *> LeafOf;
    std::unordered_map<n_t, int> TopoPos;
    std::vector<ADTNode *> LeafByPos;
    LCATable Lca;
    if (!prepareADT(R, Root, LeafOf, TopoPos, LeafByPos, Lca)) {
      return false;
    }

    initUF(Root);
    Root->UFParent = Root;
    Root->UFExpr = Exprs.one();

    if (!computePathExprDelayed(R, Root, LeafOf)) {
      return false;
    }

    Results = result_t{};
    const auto Init = Problem.initialFact();
    for (const auto &N : Problem.nodes()) {
      auto It = LeafOf.find(N);
      if (It == LeafOf.end()) {
        continue;
      }
      auto *Leaf = It->second;
      auto E = evalUF(Leaf);
      Results.ExprTo(N) = E;
      Results.IN(N) = eval(E, Init);
    }
    return true;
  }

  bool trySolveADTSimple() {
    if (const auto *R = dynamic_cast<const ReducibleProblemTy *>(&Problem)) {
      ReducibleViewProvided View(*R);
      if (View.init()) {
        return solveADTSimpleWith(View);
      }
    }

    ComputedReducibleView View;
    if (!buildComputedReducibleView(View)) {
      return false;
    }
    return solveADTSimpleWith(View);
  }

  bool trySolveADTDelayed() {
    if (const auto *R = dynamic_cast<const ReducibleProblemTy *>(&Problem)) {
      ReducibleViewProvided View(*R);
      if (View.init()) {
        return solveADTDelayedWith(View);
      }
    }

    ComputedReducibleView View;
    if (!buildComputedReducibleView(View)) {
      return false;
    }
    return solveADTDelayedWith(View);
  }

  ADTNode *newLeaf(n_t N, std::unordered_map<n_t, ADTNode *> &LeafOf) {
    ADTNodes.push_back({});
    auto *X = &ADTNodes.back();
    X->Leaf = true;
    X->FlowNode = N;
    LeafOf.emplace(N, X);
    return X;
  }

  ADTNode *newInner(ADTNode *L, ADTNode *R) {
    ADTNodes.push_back({});
    auto *X = &ADTNodes.back();
    X->Leaf = false;
    X->Left = L;
    X->Right = R;
    if (L) {
      L->Parent = X;
    }
    if (R) {
      R->Parent = X;
    }
    return X;
  }

  ADTNode *traverseADT(n_t U,
                       std::unordered_map<n_t, std::vector<n_t>> &Stacks,
                       std::unordered_map<n_t, ADTNode *> &LeafOf) {
    auto It = Stacks.find(U);
    if (It == Stacks.end()) {
      return nullptr;
    }
    ADTNode *X = newLeaf(U, LeafOf);
    auto &S = It->second;
    while (!S.empty()) {
      const auto V = S.back();
      S.pop_back();
      ADTNode *Right = traverseADT(V, Stacks, LeafOf);
      if (!Right) {
        return nullptr;
      }
      X = newInner(X, Right);
    }
    return X;
  }

  template <typename ReducibleViewT>
  bool computeFBSets(const ReducibleViewT &R, ADTNode *Root,
                     const std::unordered_map<n_t, ADTNode *> &LeafOf,
                     const std::unordered_map<n_t, int> &TopoPos,
                     const LCATable &Lca) {
    (void)Root;
    for (const auto &E : R.edges()) {
      auto ItU = LeafOf.find(E.Src);
      auto ItV = LeafOf.find(E.Dst);
      if (ItU == LeafOf.end() || ItV == LeafOf.end()) {
        return false;
      }
      auto *A = ItU->second;
      auto *B = ItV->second;
      auto *X = Lca.query(A, B);
      if (!X || X->Leaf) {
        continue;
      }

      const auto PU = TopoPos.find(E.Src);
      const auto PV = TopoPos.find(E.Dst);
      if (PU == TopoPos.end() || PV == TopoPos.end()) {
        return false;
      }
      const int SrcPos = PU->second;
      const int DstPos = PV->second;
      auto *SrcChild = childContaining(X, SrcPos);
      auto *DstChild = childContaining(X, DstPos);
      if (!SrcChild || !DstChild || SrcChild == DstChild) {
        // Edge does not cross at this composition (should belong deeper).
        continue;
      }

      if (R.isBackEdge(E.Src, E.Dst)) {
        X->B.push_back(E);
      } else {
        X->F.push_back(E);
      }
    }
    return true;
  }

  template <typename ReducibleViewT>
  bool computePathExprDelayed(const ReducibleViewT &R, ADTNode *W,
                              const std::unordered_map<n_t, ADTNode *> &LeafOf) {
    if (!W) {
      return false;
    }
    if (W->Leaf) {
      // Leaf interval: P(u,u) is handled when linking from its parent (paper Fig.
      // 6 lines 9-15). For the root leaf, handle the trivial-flowgraph case.
      W->UFExpr = Exprs.one();
      if (!W->Parent && hasSelfLoop(R, W->FlowNode)) {
        W->UFExpr = Exprs.star(Exprs.atom(R.edgeTransfer(W->FlowNode, W->FlowNode)));
      }
      return true;
    }

    assert(W->Left && W->Right);
    if (!computePathExprDelayed(R, W->Left, LeafOf)) {
      return false;
    }
    if (!computePathExprDelayed(R, W->Right, LeafOf)) {
      return false;
    }

    const auto R1 = W->Left->Entry;
    const auto R2 = W->Right->Entry;

    auto X = Exprs.zero();
    for (const auto &E : W->F) {
      auto It = LeafOf.find(E.Src);
      if (It == LeafOf.end()) {
        return false;
      }
      // Paper assumption for reducible intervals: cross forward edges target the
      // right interval entry.
      if (E.Dst != R2) {
        return false;
      }
      auto Edge = Exprs.atom(R.edgeTransfer(E.Src, E.Dst));
      X = Exprs.unite(X, Exprs.concat(evalUF(It->second), Edge));
    }

    auto Y = Exprs.zero();
    for (const auto &E : W->B) {
      auto It = LeafOf.find(E.Src);
      if (It == LeafOf.end()) {
        return false;
      }
      // Paper assumption for reducible intervals: cross back edges target the
      // left interval entry.
      if (E.Dst != R1) {
        return false;
      }
      auto Edge = Exprs.atom(R.edgeTransfer(E.Src, E.Dst));
      Y = Exprs.unite(Y, Exprs.concat(evalUF(It->second), Edge));
    }

    auto L = Exprs.star(Exprs.concat(X, Y));
    auto RPref = Exprs.concat(L, X);

    // If a child is a leaf with a self-loop, append (u->u)* to the prefix.
    if (W->Left->Leaf) {
      const auto U = W->Left->FlowNode;
      if (hasSelfLoop(R, U)) {
        L = Exprs.concat(L, Exprs.star(Exprs.atom(R.edgeTransfer(U, U))));
      }
    }
    if (W->Right->Leaf) {
      const auto U = W->Right->FlowNode;
      if (hasSelfLoop(R, U)) {
        RPref = Exprs.concat(RPref, Exprs.star(Exprs.atom(R.edgeTransfer(U, U))));
      }
    }

    linkUpdate(W, W->Left, L);
    linkUpdate(W, W->Right, RPref);
    return true;
  }

  template <typename ReducibleViewT>
  bool computePathExprSimple(const ReducibleViewT &R, ADTNode *W,
                             const std::unordered_map<n_t, ADTNode *> &LeafOf,
                             const std::vector<ADTNode *> &LeafByPos) {
    if (!W) {
      return false;
    }
    if (W->Leaf) {
      W->SimpleExpr = Exprs.one();
      if (hasSelfLoop(R, W->FlowNode)) {
        W->SimpleExpr =
            Exprs.star(Exprs.atom(R.edgeTransfer(W->FlowNode, W->FlowNode)));
      }
      return true;
    }

    assert(W->Left && W->Right);
    if (!computePathExprSimple(R, W->Left, LeafOf, LeafByPos)) {
      return false;
    }
    if (!computePathExprSimple(R, W->Right, LeafOf, LeafByPos)) {
      return false;
    }

    const auto R1 = W->Left->Entry;
    const auto R2 = W->Right->Entry;

    auto X = Exprs.zero();
    for (const auto &E : W->F) {
      auto It = LeafOf.find(E.Src);
      if (It == LeafOf.end()) {
        return false;
      }
      if (E.Dst != R2) {
        return false;
      }
      auto Edge = Exprs.atom(R.edgeTransfer(E.Src, E.Dst));
      X = Exprs.unite(X, Exprs.concat(It->second->SimpleExpr, Edge));
    }

    auto Y = Exprs.zero();
    for (const auto &E : W->B) {
      auto It = LeafOf.find(E.Src);
      if (It == LeafOf.end()) {
        return false;
      }
      if (E.Dst != R1) {
        return false;
      }
      auto Edge = Exprs.atom(R.edgeTransfer(E.Src, E.Dst));
      Y = Exprs.unite(Y, Exprs.concat(It->second->SimpleExpr, Edge));
    }

    const auto L = Exprs.star(Exprs.concat(X, Y));
    const auto RPref = Exprs.concat(L, X);

    for (int Pos = W->Left->MinPos; Pos <= W->Left->MaxPos; ++Pos) {
      auto *Leaf = LeafByPos[Pos];
      Leaf->SimpleExpr = Exprs.concat(L, Leaf->SimpleExpr);
    }
    for (int Pos = W->Right->MinPos; Pos <= W->Right->MaxPos; ++Pos) {
      auto *Leaf = LeafByPos[Pos];
      Leaf->SimpleExpr = Exprs.concat(RPref, Leaf->SimpleExpr);
    }
    return true;
  }

  template <typename ReducibleViewT>
  bool hasSelfLoop(const ReducibleViewT &R, n_t N) const {
    for (const auto &E : R.edges()) {
      if (E.Src == N && E.Dst == N) {
        return true;
      }
    }
    return false;
  }

  fact_t eval(const expr_ref_t &E, const fact_t &In) const {
    assert(E && "expression must not be null");
    switch (E->K) {
    case expr_factory_t::Kind::Zero:
      return Problem.meetIdentity();
    case expr_factory_t::Kind::One:
      return In;
    case expr_factory_t::Kind::Atom:
      return Problem.applyTransfer(*E->Transfer, In);
    case expr_factory_t::Kind::Union: {
      auto L = eval(E->L, In);
      auto R = eval(E->R, In);
      return Problem.meet(L, R);
    }
    case expr_factory_t::Kind::Concat: {
      auto Mid = eval(E->L, In);
      return eval(E->R, Mid);
    }
    case expr_factory_t::Kind::Star: {
      auto Cur = In;
      const auto Limit = Problem.maxStarIterations();
      for (std::size_t i = 0; i < Limit; ++i) {
        auto Next = Problem.meet(In, eval(E->L, Cur));
        if (Problem.equal_to(Next, Cur)) {
          return Cur;
        }
        Cur = std::move(Next);
      }
      assert(false && "star did not converge within maxStarIterations()");
      return Cur;
    }
    }
    assert(false && "unreachable");
    return Problem.meetIdentity();
  }

  std::size_t idx(const n_t &N) const {
    auto It = Index.find(N);
    assert(It != Index.end());
    return It->second;
  }

  const ProblemTy &Problem;
  EliminationOptions Opts;
  expr_factory_t Exprs;
  bool UsedADT = false;

  std::vector<n_t> Nodes;
  std::unordered_map<n_t, std::size_t> Index;
  std::vector<std::vector<expr_ref_t>> Matrix;
  result_t Results;

  std::vector<ADTNode> ADTNodes;
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_SOLVER_INTRAELIMINATIONSOLVER_H_
