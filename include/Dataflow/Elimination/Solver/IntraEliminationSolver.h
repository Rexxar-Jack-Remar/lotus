#ifndef DATAFLOW_ELIMINATION_SOLVER_INTRAELIMINATIONSOLVER_H_
#define DATAFLOW_ELIMINATION_SOLVER_INTRAELIMINATIONSOLVER_H_

#include "Dataflow/Elimination/DataFlowResult.h"
#include "Dataflow/Elimination/EliminationFramework.h"
#include "Dataflow/Elimination/Options.h"
#include "Dataflow/Elimination/PathExpression.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <unordered_map>
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
    if (Opts.Method == EliminationMethod::ADTDelayed) {
      if (trySolveADTDelayed()) {
        UsedADT = true;
        return;
      }
    }
    solveStateElimination();
  }

  [[nodiscard]] const result_t &getResults() const { return Results; }
  [[nodiscard]] bool usedADT() const { return UsedADT; }

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

  // --- ADTDelayed (paper-style) ---
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
    std::vector<typename ReducibleProblemTy::Edge> F;
    std::vector<typename ReducibleProblemTy::Edge> B;

    // Union-find link for delayed evaluation (paper Fig. 6).
    ADTNode *UFParent = nullptr;
    expr_ref_t UFExpr; // expression from UFParent's entry to this node's entry
  };

  [[nodiscard]] static bool containsPos(const ADTNode *N, int Pos) {
    return N && N->MinPos <= Pos && Pos <= N->MaxPos;
  }

  [[nodiscard]] static ADTNode *childContaining(ADTNode *W, int Pos) {
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

  [[nodiscard]] static ADTNode *lca(ADTNode *A, ADTNode *B) {
    if (!A || !B) {
      return nullptr;
    }
    while (A->Depth > B->Depth) {
      A = A->Parent;
    }
    while (B->Depth > A->Depth) {
      B = B->Parent;
    }
    while (A != B) {
      A = A->Parent;
      B = B->Parent;
      if (!A || !B) {
        return nullptr;
      }
    }
    return A;
  }

  void computeDepths(ADTNode *Root) {
    if (!Root) {
      return;
    }
    std::vector<ADTNode *> Stack;
    Root->Depth = 0;
    Stack.push_back(Root);
    while (!Stack.empty()) {
      auto *N = Stack.back();
      Stack.pop_back();
      if (N->Left) {
        N->Left->Depth = N->Depth + 1;
        Stack.push_back(N->Left);
      }
      if (N->Right) {
        N->Right->Depth = N->Depth + 1;
        Stack.push_back(N->Right);
      }
    }
  }

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

  bool trySolveADTDelayed() {
    const auto *R = dynamic_cast<const ReducibleProblemTy *>(&Problem);
    if (!R) {
      return false;
    }

    const auto Topo = R->topologicalOrder();
    if (Topo.empty()) {
      return false;
    }

    std::unordered_map<n_t, int> TopoPos;
    TopoPos.reserve(Topo.size());
    for (int i = 0; i < static_cast<int>(Topo.size()); ++i) {
      TopoPos.emplace(Topo[i], i);
    }

    // Paper assumes reducible flowgraphs: entry dominates all nodes and is first.
    if (Topo.front() != R->entry()) {
      return false;
    }

    // Build stacks s_u (paper Fig. 4).
    std::unordered_map<n_t, std::vector<n_t>> Stacks;
    Stacks.reserve(Topo.size());
    for (const auto &N : Topo) {
      (void)Stacks[N];
    }

    for (int i = static_cast<int>(Topo.size()) - 1; i >= 1; --i) {
      const auto U = Topo[i];
      const auto V = R->idom(U);
      auto It = Stacks.find(V);
      if (It == Stacks.end()) {
        return false;
      }
      It->second.push_back(U);
    }

    // Allocate ADT nodes.
    ADTNodes.clear();
    ADTNodes.reserve(2 * Topo.size());
    std::unordered_map<n_t, ADTNode *> LeafOf;
    LeafOf.reserve(Topo.size());

    auto *Root = traverseADT(R->entry(), Stacks, LeafOf);
    if (!Root) {
      return false;
    }
    Root->Parent = nullptr;
    computeDepths(Root);
    computeEntriesAndRanges(Root, TopoPos);

    // Compute F/B sets (paper Fig. 5) via NCA/LCA in ADT.
    if (!computeFBSets(*R, Root, LeafOf, TopoPos)) {
      return false;
    }

    initUF(Root);
    // Root represents ε from itself to itself.
    Root->UFParent = Root;
    Root->UFExpr = Exprs.one();

    if (!computePathExprDelayed(*R, Root, LeafOf, TopoPos)) {
      return false;
    }

    // Materialize results: P(r,u) = EVAL(leaf(u)) (paper Fig. 6 main()).
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

  bool computeFBSets(const ReducibleProblemTy &R, ADTNode *Root,
                     const std::unordered_map<n_t, ADTNode *> &LeafOf,
                     const std::unordered_map<n_t, int> &TopoPos) {
    (void)Root;
    for (const auto &E : R.edges()) {
      auto ItU = LeafOf.find(E.Src);
      auto ItV = LeafOf.find(E.Dst);
      if (ItU == LeafOf.end() || ItV == LeafOf.end()) {
        return false;
      }
      auto *A = ItU->second;
      auto *B = ItV->second;
      auto *X = lca(A, B);
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

  bool computePathExprDelayed(const ReducibleProblemTy &R, ADTNode *W,
                              const std::unordered_map<n_t, ADTNode *> &LeafOf,
                              const std::unordered_map<n_t, int> &TopoPos) {
    if (!W) {
      return false;
    }
    if (W->Leaf) {
      // Leaf interval: P(u,u) is handled when linking from its parent (paper Fig.
      // 6 lines 9-15). Here we keep the base as ε (identity).
      W->UFExpr = Exprs.one();
      return true;
    }

    assert(W->Left && W->Right);
    if (!computePathExprDelayed(R, W->Left, LeafOf, TopoPos)) {
      return false;
    }
    if (!computePathExprDelayed(R, W->Right, LeafOf, TopoPos)) {
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

  [[nodiscard]] bool hasSelfLoop(const ReducibleProblemTy &R, n_t N) const {
    for (const auto &E : R.edges()) {
      if (E.Src == N && E.Dst == N) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] fact_t eval(const expr_ref_t &E, const fact_t &In) const {
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

  [[nodiscard]] std::size_t idx(const n_t &N) const {
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
