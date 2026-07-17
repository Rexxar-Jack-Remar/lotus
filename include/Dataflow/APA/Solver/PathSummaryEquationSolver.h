#ifndef DATAFLOW_APA_SOLVER_PATHSUMMARYEQUATIONSOLVER_H_
#define DATAFLOW_APA_SOLVER_PATHSUMMARYEQUATIONSOLVER_H_

#include "Dataflow/APA/Core/PathExpr.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elimination {

// A graph of left-linear algebraic summary equations:
//
//   X_u = base_u U (W_u,v . X_v)
//
// Nodes are intended to represent interprocedural summary instances such as
// (function, call-string context). Edge weights are APA path expressions, not
// ordinary scheduler dependencies. The solver computes the closed-form regular
// path-expression solution by solving SCCs in dependency order.
template <typename KeyT, typename TransferT>
class PathSummaryEquationGraph final {
public:
  using key_t = KeyT;
  using transfer_t = TransferT;
  using expr_factory_t = PathExprFactory<TransferT>;
  using expr_ref_t = typename expr_factory_t::Ref;

  struct Node final {
    key_t Key;
    expr_ref_t Base;
  };

  struct Edge final {
    std::size_t Source = 0;
    std::size_t Target = 0;
    expr_ref_t Weight;
  };

  std::size_t addNode(key_t Key, expr_ref_t Base = {}) {
    auto It = Index.find(Key);
    if (It != Index.end()) {
      if (Base) {
        Nodes[It->second].Base = Base;
      }
      return It->second;
    }
    const std::size_t Id = Nodes.size();
    if (!Base) {
      Base = Exprs.zero();
    }
    Nodes.push_back(Node{std::move(Key), Base});
    Index.emplace(Nodes.back().Key, Id);
    return Id;
  }

  void setBase(const key_t &Key, expr_ref_t Base) {
    Nodes[addNode(Key)].Base = Base ? Base : Exprs.zero();
  }

  void addEdge(const key_t &Source, const key_t &Target, expr_ref_t Weight) {
    const std::size_t SourceId = addNode(Source);
    const std::size_t TargetId = addNode(Target);
    Edges.push_back(Edge{SourceId, TargetId, Weight ? Weight : Exprs.one()});
  }

  const std::vector<Node> &nodes() const { return Nodes; }
  const std::vector<Edge> &edges() const { return Edges; }

  const expr_factory_t &exprs() const { return Exprs; }
  expr_factory_t &exprs() { return Exprs; }

private:
  expr_factory_t Exprs;
  std::vector<Node> Nodes;
  std::vector<Edge> Edges;
  std::map<key_t, std::size_t> Index;
};

enum class PathSummaryEquationDirection {
  DependencyPrefix,
  ForwardPath,
};

struct PathSummaryEquationOptions final {
  PathSummaryEquationDirection Direction =
      PathSummaryEquationDirection::DependencyPrefix;
};

struct PathSummaryEquationDiagnostics final {
  std::size_t node_count = 0;
  std::size_t edge_count = 0;
  std::size_t scc_count = 0;
  std::size_t cyclic_scc_count = 0;
};

template <typename KeyT, typename TransferT>
class PathSummaryEquationResult final {
public:
  using expr_ref_t = typename PathExprFactory<TransferT>::Ref;

  const expr_ref_t *lookup(const KeyT &Key) const {
    auto It = Summaries.find(Key);
    return It == Summaries.end() ? nullptr : &It->second;
  }

  const std::map<KeyT, expr_ref_t> &summaries() const { return Summaries; }
  const PathSummaryEquationDiagnostics &diagnostics() const {
    return Diagnostics;
  }

private:
  template <typename K, typename T> friend class PathSummaryEquationSolver;

  std::map<KeyT, expr_ref_t> Summaries;
  PathSummaryEquationDiagnostics Diagnostics;
};

template <typename KeyT, typename TransferT>
class PathSummaryEquationSolver final {
public:
  using graph_t = PathSummaryEquationGraph<KeyT, TransferT>;
  using expr_factory_t = typename graph_t::expr_factory_t;
  using expr_ref_t = typename graph_t::expr_ref_t;
  using result_t = PathSummaryEquationResult<KeyT, TransferT>;

  explicit PathSummaryEquationSolver(const graph_t &Graph,
                                     PathSummaryEquationOptions Options = {})
      : Graph(Graph), Options(Options) {}

  result_t solve() {
    Result = result_t{};
    Result.Diagnostics.node_count = Graph.nodes().size();
    Result.Diagnostics.edge_count = Graph.edges().size();
    SummaryByNode.assign(Graph.nodes().size(), {});
    buildAdjacency();
    computeSCCs();
    if (Options.Direction == PathSummaryEquationDirection::ForwardPath) {
      solveForwardComponentsInDependencyOrder();
    } else {
      solveComponentsInDependencyOrder();
    }
    for (std::size_t I = 0; I < Graph.nodes().size(); ++I) {
      if (SummaryByNode[I]) {
        Result.Summaries.emplace(Graph.nodes()[I].Key, SummaryByNode[I]);
      }
    }
    return Result;
  }

private:
  struct Component final {
    std::vector<std::size_t> Nodes;
  };

  expr_ref_t zero() const { return Graph.exprs().zero(); }

  expr_ref_t one() const { return Graph.exprs().one(); }

  expr_ref_t unite(const expr_ref_t &A, const expr_ref_t &B) const {
    return Graph.exprs().unite(A, B);
  }

  expr_ref_t concat(const expr_ref_t &A, const expr_ref_t &B) const {
    return Graph.exprs().concat(A, B);
  }

  expr_ref_t star(const expr_ref_t &A) const { return Graph.exprs().star(A); }

  void buildAdjacency() {
    const std::size_t N = Graph.nodes().size();
    OutEdges.assign(N, {});
    InEdges.assign(N, {});
    for (std::size_t I = 0; I < Graph.edges().size(); ++I) {
      const auto &E = Graph.edges()[I];
      assert(E.Source < N && E.Target < N);
      OutEdges[E.Source].push_back(I);
      InEdges[E.Target].push_back(I);
    }
  }

  void computeSCCs() {
    const std::size_t N = Graph.nodes().size();
    Components.clear();
    ComponentOf.assign(N, static_cast<std::size_t>(-1));

    std::vector<int> Index(N, -1);
    std::vector<int> LowLink(N, 0);
    std::vector<std::size_t> Stack;
    std::vector<bool> OnStack(N, false);
    int NextIndex = 0;

    auto StrongConnect = [&](auto &&Self, std::size_t V) -> void {
      Index[V] = LowLink[V] = NextIndex++;
      Stack.push_back(V);
      OnStack[V] = true;

      for (std::size_t EdgeId : OutEdges[V]) {
        const std::size_t W = Graph.edges()[EdgeId].Target;
        if (Index[W] == -1) {
          Self(Self, W);
          LowLink[V] = std::min(LowLink[V], LowLink[W]);
        } else if (OnStack[W]) {
          LowLink[V] = std::min(LowLink[V], Index[W]);
        }
      }

      if (LowLink[V] != Index[V]) {
        return;
      }

      Component C;
      while (true) {
        const std::size_t W = Stack.back();
        Stack.pop_back();
        OnStack[W] = false;
        ComponentOf[W] = Components.size();
        C.Nodes.push_back(W);
        if (W == V) {
          break;
        }
      }
      std::sort(C.Nodes.begin(), C.Nodes.end());
      Components.push_back(std::move(C));
    };

    for (std::size_t V = 0; V < N; ++V) {
      if (Index[V] == -1) {
        StrongConnect(StrongConnect, V);
      }
    }

    Result.Diagnostics.scc_count = Components.size();
    for (const auto &C : Components) {
      if (isCyclic(C)) {
        ++Result.Diagnostics.cyclic_scc_count;
      }
    }
  }

  bool isCyclic(const Component &C) const {
    if (C.Nodes.size() > 1) {
      return true;
    }
    const std::size_t Node = C.Nodes.front();
    for (std::size_t EdgeId : OutEdges[Node]) {
      if (Graph.edges()[EdgeId].Target == Node) {
        return true;
      }
    }
    return false;
  }

  void solveComponentsInDependencyOrder() {
    const std::size_t CCount = Components.size();
    std::vector<std::set<std::size_t>> DependsOn(CCount);
    std::vector<std::set<std::size_t>> Users(CCount);
    for (const auto &E : Graph.edges()) {
      const std::size_t SourceC = ComponentOf[E.Source];
      const std::size_t TargetC = ComponentOf[E.Target];
      if (SourceC == TargetC) {
        continue;
      }
      DependsOn[SourceC].insert(TargetC);
      Users[TargetC].insert(SourceC);
    }

    std::vector<std::size_t> Unresolved(CCount, 0);
    std::vector<std::size_t> Ready;
    for (std::size_t C = 0; C < CCount; ++C) {
      Unresolved[C] = DependsOn[C].size();
      if (Unresolved[C] == 0) {
        Ready.push_back(C);
      }
    }

    while (!Ready.empty()) {
      std::sort(Ready.begin(), Ready.end());
      for (std::size_t C : Ready) {
        solveComponent(Components[C]);
      }

      std::vector<std::size_t> NextReady;
      for (std::size_t C : Ready) {
        for (std::size_t User : Users[C]) {
          assert(Unresolved[User] != 0 &&
                 "component dependency count underflow");
          --Unresolved[User];
          if (Unresolved[User] == 0) {
            NextReady.push_back(User);
          }
        }
      }
      Ready.swap(NextReady);
    }
  }

  void solveComponent(const Component &C) {
    if (!isCyclic(C)) {
      solveAcyclicSingleton(C.Nodes.front());
      return;
    }
    solveCyclicComponent(C);
  }

  void solveForwardComponentsInDependencyOrder() {
    const std::size_t CCount = Components.size();
    std::vector<std::set<std::size_t>> Preds(CCount);
    std::vector<std::set<std::size_t>> Users(CCount);
    for (const auto &E : Graph.edges()) {
      const std::size_t SourceC = ComponentOf[E.Source];
      const std::size_t TargetC = ComponentOf[E.Target];
      if (SourceC == TargetC) {
        continue;
      }
      Preds[TargetC].insert(SourceC);
      Users[SourceC].insert(TargetC);
    }

    std::vector<std::size_t> Unresolved(CCount, 0);
    std::vector<std::size_t> Ready;
    for (std::size_t C = 0; C < CCount; ++C) {
      Unresolved[C] = Preds[C].size();
      if (Unresolved[C] == 0) {
        Ready.push_back(C);
      }
    }

    while (!Ready.empty()) {
      std::sort(Ready.begin(), Ready.end());
      for (std::size_t C : Ready) {
        solveForwardComponent(Components[C]);
      }

      std::vector<std::size_t> NextReady;
      for (std::size_t C : Ready) {
        for (std::size_t User : Users[C]) {
          assert(Unresolved[User] != 0 &&
                 "component dependency count underflow");
          --Unresolved[User];
          if (Unresolved[User] == 0) {
            NextReady.push_back(User);
          }
        }
      }
      Ready.swap(NextReady);
    }
  }

  void solveForwardComponent(const Component &C) {
    if (!isCyclic(C)) {
      solveForwardAcyclicSingleton(C.Nodes.front());
      return;
    }
    solveForwardCyclicComponent(C);
  }

  expr_ref_t forwardBaseForNode(std::size_t Node) {
    expr_ref_t Base = Graph.nodes()[Node].Base;
    if (!Base) {
      Base = zero();
    }
    for (std::size_t EdgeId : InEdges[Node]) {
      const auto &E = Graph.edges()[EdgeId];
      if (ComponentOf[E.Source] == ComponentOf[Node]) {
        continue;
      }
      assert(SummaryByNode[E.Source] &&
             "predecessor component must be solved first");
      Base = unite(Base, concat(SummaryByNode[E.Source], E.Weight));
    }
    return Base;
  }

  void solveForwardAcyclicSingleton(std::size_t Node) {
    SummaryByNode[Node] = forwardBaseForNode(Node);
  }

  void solveForwardCyclicComponent(const Component &C) {
    const std::size_t N = C.Nodes.size();
    std::unordered_map<std::size_t, std::size_t> LocalIndex;
    LocalIndex.reserve(N);
    for (std::size_t I = 0; I < N; ++I) {
      LocalIndex.emplace(C.Nodes[I], I);
    }

    std::vector<expr_ref_t> Base(N, zero());
    std::vector<std::vector<expr_ref_t>> Matrix(N, std::vector<expr_ref_t>(N));
    for (std::size_t I = 0; I < N; ++I) {
      Base[I] = forwardBaseForNode(C.Nodes[I]);
      for (std::size_t J = 0; J < N; ++J) {
        Matrix[I][J] = I == J ? one() : zero();
      }
    }

    for (std::size_t I = 0; I < N; ++I) {
      const std::size_t Node = C.Nodes[I];
      for (std::size_t EdgeId : OutEdges[Node]) {
        const auto &E = Graph.edges()[EdgeId];
        auto LocalTarget = LocalIndex.find(E.Target);
        if (LocalTarget == LocalIndex.end()) {
          continue;
        }
        Matrix[I][LocalTarget->second] =
            unite(Matrix[I][LocalTarget->second], E.Weight);
      }
    }

    for (std::size_t K = 0; K < N; ++K) {
      std::vector<expr_ref_t> ColK(N);
      std::vector<expr_ref_t> RowK(N);
      for (std::size_t I = 0; I < N; ++I) {
        ColK[I] = Matrix[I][K];
      }
      for (std::size_t J = 0; J < N; ++J) {
        RowK[J] = Matrix[K][J];
      }

      const auto KStar = star(Matrix[K][K]);
      for (std::size_t I = 0; I < N; ++I) {
        if (expr_factory_t::isZero(ColK[I])) {
          continue;
        }
        for (std::size_t J = 0; J < N; ++J) {
          if (expr_factory_t::isZero(RowK[J])) {
            continue;
          }
          auto Via = concat(ColK[I], KStar);
          Via = concat(Via, RowK[J]);
          Matrix[I][J] = unite(Matrix[I][J], Via);
        }
      }
    }

    for (std::size_t J = 0; J < N; ++J) {
      expr_ref_t Summary = zero();
      for (std::size_t I = 0; I < N; ++I) {
        if (expr_factory_t::isZero(Base[I]) ||
            expr_factory_t::isZero(Matrix[I][J])) {
          continue;
        }
        Summary = unite(Summary, concat(Base[I], Matrix[I][J]));
      }
      SummaryByNode[C.Nodes[J]] = Summary;
    }
  }

  void solveAcyclicSingleton(std::size_t Node) {
    expr_ref_t Summary = Graph.nodes()[Node].Base;
    if (!Summary) {
      Summary = zero();
    }
    for (std::size_t EdgeId : OutEdges[Node]) {
      const auto &E = Graph.edges()[EdgeId];
      if (ComponentOf[E.Target] == ComponentOf[Node]) {
        continue;
      }
      assert(SummaryByNode[E.Target] &&
             "dependency component must be solved first");
      Summary = unite(Summary, concat(E.Weight, SummaryByNode[E.Target]));
    }
    SummaryByNode[Node] = Summary;
  }

  void solveCyclicComponent(const Component &C) {
    const std::size_t N = C.Nodes.size();
    std::unordered_map<std::size_t, std::size_t> LocalIndex;
    LocalIndex.reserve(N);
    for (std::size_t I = 0; I < N; ++I) {
      LocalIndex.emplace(C.Nodes[I], I);
    }

    std::vector<expr_ref_t> Base(N, zero());
    std::vector<std::vector<expr_ref_t>> Matrix(N, std::vector<expr_ref_t>(N));
    for (std::size_t I = 0; I < N; ++I) {
      Base[I] = Graph.nodes()[C.Nodes[I]].Base;
      if (!Base[I]) {
        Base[I] = zero();
      }
      for (std::size_t J = 0; J < N; ++J) {
        Matrix[I][J] = I == J ? one() : zero();
      }
    }

    for (std::size_t I = 0; I < N; ++I) {
      const std::size_t Node = C.Nodes[I];
      for (std::size_t EdgeId : OutEdges[Node]) {
        const auto &E = Graph.edges()[EdgeId];
        auto LocalTarget = LocalIndex.find(E.Target);
        if (LocalTarget != LocalIndex.end()) {
          Matrix[I][LocalTarget->second] =
              unite(Matrix[I][LocalTarget->second], E.Weight);
          continue;
        }

        assert(SummaryByNode[E.Target] &&
               "dependency component must be solved first");
        Base[I] = unite(Base[I], concat(E.Weight, SummaryByNode[E.Target]));
      }
    }

    for (std::size_t K = 0; K < N; ++K) {
      std::vector<expr_ref_t> ColK(N);
      std::vector<expr_ref_t> RowK(N);
      for (std::size_t I = 0; I < N; ++I) {
        ColK[I] = Matrix[I][K];
      }
      for (std::size_t J = 0; J < N; ++J) {
        RowK[J] = Matrix[K][J];
      }

      const auto KStar = star(Matrix[K][K]);
      for (std::size_t I = 0; I < N; ++I) {
        if (expr_factory_t::isZero(ColK[I])) {
          continue;
        }
        for (std::size_t J = 0; J < N; ++J) {
          if (expr_factory_t::isZero(RowK[J])) {
            continue;
          }
          auto Via = concat(ColK[I], KStar);
          Via = concat(Via, RowK[J]);
          Matrix[I][J] = unite(Matrix[I][J], Via);
        }
      }
    }

    for (std::size_t I = 0; I < N; ++I) {
      expr_ref_t Summary = zero();
      for (std::size_t J = 0; J < N; ++J) {
        if (expr_factory_t::isZero(Matrix[I][J])) {
          continue;
        }
        Summary = unite(Summary, concat(Matrix[I][J], Base[J]));
      }
      SummaryByNode[C.Nodes[I]] = Summary;
    }
  }

  const graph_t &Graph;
  PathSummaryEquationOptions Options;
  std::vector<std::vector<std::size_t>> OutEdges;
  std::vector<std::vector<std::size_t>> InEdges;
  std::vector<Component> Components;
  std::vector<std::size_t> ComponentOf;
  std::vector<expr_ref_t> SummaryByNode;
  result_t Result;
};

} // namespace elimination

#endif // DATAFLOW_APA_SOLVER_PATHSUMMARYEQUATIONSOLVER_H_
