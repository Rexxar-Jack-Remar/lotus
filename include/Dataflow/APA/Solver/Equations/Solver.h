#pragma once

#include "Dataflow/APA/Solver/Elimination/DenseClosure.h"
#include "Dataflow/APA/Solver/Elimination/SparseSolver.h"
#include "Dataflow/APA/Solver/Equations/Graph.h"
#include "Dataflow/APA/Solver/Equations/Result.h"
#include "Dataflow/APA/Solver/Graph/SCC.h"

#include <cassert>
#include <set>
#include <unordered_map>

namespace elimination {

// SCC scheduling and boundary substitution only. The shared elimination
// kernels own cyclic construction; graph, options and results have separate
// headers.
template <typename KeyT, typename TransferT>
class PathSummaryEquationSolver final {
public:
  using graph_t = PathSummaryEquationGraph<KeyT, TransferT>;
  using expr_factory_t = typename graph_t::expr_factory_t;
  using expr_ref_t = typename graph_t::expr_ref_t;
  using result_t = PathSummaryEquationResult<KeyT, TransferT>;

  explicit PathSummaryEquationSolver(const graph_t &Graph,
                                     PathSummaryEquationOptions Options = {})
      : Graph(Graph), Options(std::move(Options)) {}

  result_t solve() {
    Result = result_t{};
    const auto N = Graph.nodes().size();
    const auto InitialAllocations = Graph.exprs().allocationCount();
    const auto InitialStars = Graph.exprs().starAllocationCount();
    Result.Diagnostics.node_count = N;
    Result.Diagnostics.edge_count = Graph.edges().size();
    SummaryByNode.assign(N, {});
    order::validateOrderOptions(Options.Order, OrderingPolicy::Default, 0);
    OutEdges.assign(N, {});
    InEdges.assign(N, {});
    std::vector<std::vector<std::size_t>> Succ(N);
    for (std::size_t I = 0; I < Graph.edges().size(); ++I) {
      const auto &Edge = Graph.edges()[I];
      OutEdges[Edge.Source].push_back(I);
      InEdges[Edge.Target].push_back(I);
      Succ[Edge.Source].push_back(Edge.Target);
    }
    SCCs = detail::computeSCCs(Succ);
    Result.Diagnostics.scc_count = SCCs.Components.size();
    for (const auto &C : SCCs.Components) {
      Result.Diagnostics.cyclic_scc_count += isCyclic(C);
    }
    solveComponents();
    for (std::size_t I = 0; I < N; ++I) {
      Result.Summaries.emplace(Graph.nodes()[I].Key, SummaryByNode[I]);
    }
    auto &D = Result.Diagnostics.ordering;
    D.allocated_nodes = Graph.exprs().allocationCount();
    D.allocated_stars = Graph.exprs().starAllocationCount();
    D.initial_allocated_nodes += InitialAllocations;
    D.boundary_allocated_nodes = D.allocated_nodes - D.initial_allocated_nodes -
                                 D.elimination_allocated_nodes -
                                 D.backsubstitution_allocated_nodes;
    Result.Diagnostics.new_allocated_nodes =
        Graph.exprs().allocationCount() - InitialAllocations;
    Result.Diagnostics.new_allocated_stars =
        Graph.exprs().starAllocationCount() - InitialStars;
    if (Options.Order.MeasureLiveNodes || Options.Order.RecordTrace) {
      const auto AllSummaries = detail::countDag<TransferT>(SummaryByNode);
      D.peak_live_nodes = std::max(D.peak_live_nodes, AllSummaries.first);
      D.peak_live_edges = std::max(D.peak_live_edges, AllSummaries.second);
    }
    return Result;
  }

private:
  using Component = detail::SCCDecomposition::Component;
  bool forward() const {
    return Options.Direction == PathSummaryEquationDirection::ForwardPath;
  }
  expr_ref_t zero() const { return Graph.exprs().zero(); }
  bool isCyclic(const Component &C) const {
    if (C.Nodes.size() > 1)
      return true;
    for (auto I : OutEdges[C.Nodes.front()]) {
      if (Graph.edges()[I].Target == C.Nodes.front())
        return true;
    }
    return false;
  }

  void solveComponents() {
    const auto N = SCCs.Components.size();
    std::vector<std::set<std::size_t>> Dependencies(N), Users(N);
    for (const auto &Edge : Graph.edges()) {
      const auto Source = SCCs.ComponentOf[Edge.Source];
      const auto Target = SCCs.ComponentOf[Edge.Target];
      if (Source == Target)
        continue;
      const auto Consumer = forward() ? Target : Source;
      const auto Provider = forward() ? Source : Target;
      Dependencies[Consumer].insert(Provider);
      Users[Provider].insert(Consumer);
    }
    std::vector<std::size_t> Pending(N), Ready;
    for (std::size_t I = 0; I < N; ++I) {
      Pending[I] = Dependencies[I].size();
      if (Pending[I] == 0)
        Ready.push_back(I);
    }
    while (!Ready.empty()) {
      std::sort(Ready.begin(), Ready.end());
      for (auto I : Ready)
        solveComponent(SCCs.Components[I]);
      std::vector<std::size_t> Next;
      for (auto I : Ready) {
        for (auto User : Users[I]) {
          assert(Pending[User] != 0);
          if (--Pending[User] == 0)
            Next.push_back(User);
        }
      }
      Ready = std::move(Next);
    }
  }

  expr_ref_t boundaryForNode(std::size_t Node) const {
    auto Base = Graph.nodes()[Node].Base;
    if (!Base)
      Base = zero();
    const auto &Edges = forward() ? InEdges[Node] : OutEdges[Node];
    for (auto I : Edges) {
      const auto &Edge = Graph.edges()[I];
      const auto Provider = forward() ? Edge.Source : Edge.Target;
      if (SCCs.ComponentOf[Provider] == SCCs.ComponentOf[Node])
        continue;
      assert(SummaryByNode[Provider] &&
             "boundary provider must be solved first");
      const auto Term =
          forward()
              ? Graph.exprs().concat(SummaryByNode[Provider], Edge.Weight)
              : Graph.exprs().concat(Edge.Weight, SummaryByNode[Provider]);
      Base = Graph.exprs().unite(Base, Term);
    }
    return Base;
  }

  void solveComponent(const Component &C) {
    if (!isCyclic(C)) {
      SummaryByNode[C.Nodes.front()] = boundaryForNode(C.Nodes.front());
      return;
    }
    if (Options.Ordering != OrderingPolicy::Default ||
        Options.Order.UseSparseElimination) {
      solveSparse(C, false);
    } else if (forward() && C.Nodes.size() > 16) {
      solveSparse(
          C, true); // Historical sparse min-fill, with no identity self-edges.
    } else {
      solveDense(C);
    }
  }

  std::unordered_map<std::size_t, std::size_t>
  localIndex(const Component &C) const {
    std::unordered_map<std::size_t, std::size_t> Index;
    for (std::size_t I = 0; I < C.Nodes.size(); ++I)
      Index.emplace(C.Nodes[I], I);
    return Index;
  }

  void solveSparse(const Component &C, bool NativeMinFill) {
    const auto N = C.Nodes.size();
    const auto Index = localIndex(C);
    std::vector<expr_ref_t> Base(N);
    for (std::size_t I = 0; I < N; ++I)
      Base[I] = boundaryForNode(C.Nodes[I]);
    auto Policy = NativeMinFill ? OrderingPolicy::MinFill : Options.Ordering;
    auto Ordering = Options.Order;
    if (Policy == OrderingPolicy::Default) {
      Policy = OrderingPolicy::Explicit;
      Ordering.ExplicitOrder.resize(N);
      std::iota(Ordering.ExplicitOrder.begin(), Ordering.ExplicitOrder.end(),
                0);
    }
    detail::SparseElimination<TransferT> Solver(Graph.exprs(), std::move(Base),
                                                Policy, Ordering, forward(),
                                                C.Nodes, !NativeMinFill);
    for (std::size_t I = 0; I < N; ++I) {
      for (auto E : OutEdges[C.Nodes[I]]) {
        const auto &Edge = Graph.edges()[E];
        auto Target = Index.find(Edge.Target);
        if (Target == Index.end())
          continue;
        Solver.addEdge(forward() ? I : Target->second,
                       forward() ? Target->second : I, Edge.Weight);
      }
    }
    auto Summaries = Solver.solve();
    for (std::size_t I = 0; I < N; ++I)
      SummaryByNode[C.Nodes[I]] = Summaries[I];
    Result.Diagnostics.ordering.append(Solver.diagnostics());
  }

  void solveDense(const Component &C) {
    const auto N = C.Nodes.size();
    const auto Index = localIndex(C);
    const auto &Factory = Graph.exprs();
    std::vector<expr_ref_t> Base(N);
    detail::DenseMatrix<TransferT> Matrix(N,
                                          std::vector<expr_ref_t>(N, zero()));
    for (std::size_t I = 0; I < N; ++I) {
      Base[I] = boundaryForNode(C.Nodes[I]);
      Matrix[I][I] = Factory.one();
      for (auto E : OutEdges[C.Nodes[I]]) {
        const auto &Edge = Graph.edges()[E];
        auto Target = Index.find(Edge.Target);
        if (Target != Index.end()) {
          Matrix[I][Target->second] =
              Factory.unite(Matrix[I][Target->second], Edge.Weight);
        }
      }
    }
    detail::closeDenseMatrix(Matrix, Factory);
    for (std::size_t I = 0; I < N; ++I) {
      auto Summary = zero();
      for (std::size_t J = 0; J < N; ++J) {
        const auto &Weight = Matrix[forward() ? J : I][forward() ? I : J];
        const auto Term = forward() ? Factory.concat(Base[J], Weight)
                                    : Factory.concat(Weight, Base[J]);
        Summary = Factory.unite(Summary, Term);
      }
      SummaryByNode[C.Nodes[I]] = Summary;
    }
  }

  const graph_t &Graph;
  PathSummaryEquationOptions Options;
  std::vector<std::vector<std::size_t>> OutEdges, InEdges;
  detail::SCCDecomposition SCCs;
  std::vector<expr_ref_t> SummaryByNode;
  result_t Result;
};

} // namespace elimination
