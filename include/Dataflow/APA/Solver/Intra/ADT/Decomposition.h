#pragma once

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Solver/Intra/ADT/Reducibility.h"
#include "Dataflow/APA/Solver/Intra/ADT/Types.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace elimination::detail {

template <typename AnalysisTypesT> class ADTDecomposition final {
public:
  using ProblemTy = IntraEliminationProblem<AnalysisTypesT>;
  using ReducibleProblemTy = IntraReducibleEliminationProblem<AnalysisTypesT>;
  using n_t = typename ProblemTy::n_t;
  using transfer_t = typename ProblemTy::transfer_t;
  using expr_factory_t = PathExprFactory<transfer_t>;
  using expr_ref_t = typename expr_factory_t::Ref;
  using Types = ADTTypes<n_t, expr_ref_t>;
  using Edge = typename Types::Edge;
  using ADTNode = typename Types::ADTNode;
  using LCATable = typename Types::LCATable;
  using ReducibilityTy = Reducibility<AnalysisTypesT>;

  ADTDecomposition(const ProblemTy &Problem, expr_factory_t &Exprs,
                   SolveDiagnostics &Diagnostics)
      : Reducible(Problem, Diagnostics), Problem(Problem), Exprs(Exprs) {}

  ReducibilityTy Reducible;

  // Populate interval entries and leaf ranges after the ADT shape is built.
  void computeEntriesAndRanges(ADTNode *N,
                               const std::unordered_map<n_t, int> &TopoPos) {
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

  // Compose delayed prefixes on demand, compressing the path to the ADT root.
  expr_ref_t evalUF(ADTNode *X) {
    assert(X);
    if (X->UFParent == X) {
      return X->UFExpr;
    }
    auto ParentExpr = evalUF(X->UFParent);
    X->UFExpr = Exprs.concat(ParentExpr, X->UFExpr);
    X->UFParent = X->UFParent->UFParent;
    return X->UFExpr;
  }

  template <typename ReducibleViewT>
  bool prepareADT(const ReducibleViewT &R, ADTNode *&Root,
                  std::unordered_map<n_t, ADTNode *> &LeafOf,
                  std::unordered_map<n_t, int> &TopoPos,
                  std::vector<ADTNode *> &LeafByPos, LCATable &Lca) {
    const auto &Topo = R.topologicalOrder();
    if (Topo.empty()) {
      return Reducible.rejectADT(ADTRejectionReason::EmptyTopologicalOrder);
    }

    TopoPos.clear();
    TopoPos.reserve(Topo.size());
    for (int i = 0; i < static_cast<int>(Topo.size()); ++i) {
      TopoPos.emplace(Topo[i], i);
    }

    if (Topo.front() != Problem.entry()) {
      return Reducible.rejectADT(ADTRejectionReason::EntryNotFirst);
    }

    for (const auto &N : Problem.nodes()) {
      if (TopoPos.find(N) == TopoPos.end()) {
        return Reducible.rejectADT(ADTRejectionReason::MissingTopologicalNode);
      }
    }

    // Build the child stacks from the dominator tree as in the annotated
    // decomposition tree construction.
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
        return Reducible.rejectADT(
            ADTRejectionReason::InvalidImmediateDominator);
      }
      It->second.push_back(U);
    }

    // Allocate all ADT nodes from a single backing vector so pointers remain
    // stable once reserve() has been applied.
    ADTNodes.clear();
    ADTNodes.reserve(2 * Topo.size());
    LeafOf.clear();
    LeafOf.reserve(Topo.size());

    Root = traverseADT(Problem.entry(), Stacks, LeafOf);
    if (!Root) {
      return Reducible.rejectADT(ADTRejectionReason::ADTConstructionFailed);
    }
    Root->Parent = nullptr;
    computeEntriesAndRanges(Root, TopoPos);

    LeafByPos.assign(Topo.size(), nullptr);
    for (const auto &It : LeafOf) {
      const auto PosIt = TopoPos.find(It.first);
      if (PosIt == TopoPos.end()) {
        return Reducible.rejectADT(ADTRejectionReason::MissingTopologicalNode);
      }
      LeafByPos[PosIt->second] = It.second;
    }
    for (auto *Leaf : LeafByPos) {
      if (!Leaf) {
        return Reducible.rejectADT(ADTRejectionReason::MissingADTLeaf);
      }
    }

    Lca.build(Root);
    if (!computeFBSets(R, Root, LeafOf, TopoPos, Lca)) {
      return Reducible.rejectADT(ADTRejectionReason::EdgeClassificationFailed);
    }
    buildSelfLoopCache(R);
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

  ADTNode *traverseADT(n_t U, std::unordered_map<n_t, std::vector<n_t>> &Stacks,
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
    // Classify each CFG edge at the lowest ADT composition node where it
    // crosses from one child interval to the other.
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
  bool hasSelfLoop(const ReducibleViewT &R, n_t N) const {
    (void)R;
    auto It = SelfLoops.find(N);
    if (It == SelfLoops.end()) {
      return false;
    }
    return It->second;
  }

private:
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

  template <typename ReducibleViewT>
  void buildSelfLoopCache(const ReducibleViewT &R) {
    SelfLoops.clear();
    for (const auto &E : R.edges()) {
      if (E.Src == E.Dst) {
        SelfLoops[E.Src] = true;
      }
    }
  }
  const ProblemTy &Problem;
  expr_factory_t &Exprs;
  std::vector<ADTNode> ADTNodes;
  std::unordered_map<n_t, bool> SelfLoops;
};

} // namespace elimination::detail
