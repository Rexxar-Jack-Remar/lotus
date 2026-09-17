#pragma once

#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Solver/Elimination/DagMetrics.h"
#include "Dataflow/APA/Solver/Ordering/Selector.h"
#include "Dataflow/APA/Solver/Ordering/Signals.h"

#include <cassert>
#include <map>
#include <unordered_set>

namespace elimination {
namespace detail {

// Solves X_v = Base_v U U_{u->v} X_u . Weight[u,v]. For dependency-prefix
// equations, transpose the edges and set Forward=false to reverse composition.
// Removed equations are retained for reverse back-substitution, so every query
// point is recovered without keeping eliminated rows in the live graph.
template <typename TransferT> class SparseElimination final {
public:
  using Factory = PathExprFactory<TransferT>;
  using Ref = typename Factory::Ref;
  using Adjacency = std::map<std::size_t, Ref>;

  SparseElimination(const Factory &Exprs, std::vector<Ref> Base,
                    OrderingPolicy Policy, OrderPolicyOptions Options = {},
                    bool Forward = true, std::vector<std::size_t> NodeIds = {},
                    bool IdentityDiagonal = true)
      : Exprs(Exprs), Base(std::move(Base)), Policy(Policy),
        Options(std::move(Options)), Forward(Forward),
        NodeIds(std::move(NodeIds)), Succ(this->Base.size()),
        Pred(this->Base.size()), SelfWeight(this->Base.size()),
        Dead(this->Base.size(), false),
        AllocationBaseline(Exprs.allocationCount()),
        StarBaseline(Exprs.starAllocationCount()) {
    order::validateOrderOptions(this->Options, Policy, this->Base.size());
    for (auto &Root : this->Base) {
      if (!Root) {
        Root = Exprs.zero();
      }
    }
    // Match the summary-matrix invariant: diagonal entries initially include
    // the empty path. These identity self-edges never count towards P or Q.
    if (IdentityDiagonal) {
      for (std::size_t I = 0; I < this->Base.size(); ++I) {
        addEdge(I, I, Exprs.one());
      }
    }
  }

  void addEdge(std::size_t Source, std::size_t Target, const Ref &Weight) {
    if (Source >= Base.size() || Target >= Base.size()) {
      throw std::invalid_argument(
          "APA sparse edge endpoint is outside the graph");
    }
    if (!Weight || Factory::isZero(Weight)) {
      return;
    }
    auto It = Succ[Source].find(Target);
    Ref Merged =
        It == Succ[Source].end() ? Weight : Exprs.unite(It->second, Weight);
    Succ[Source][Target] = Merged;
    Pred[Target][Source] = Merged;
    if (Source == Target) {
      SelfWeight[Source] = Merged;
    }
    warmSize(Merged);
  }

  OrderSignals signals(std::size_t Node) const {
    return order::collectSignals<TransferT>(
        Node, Pred[Node], Succ[Node], SelfWeight[Node], Exprs, Options,
        order::requirements(Policy),
        [&](std::size_t Source, std::size_t Target) {
          return Succ[Source].count(Target) != 0;
        });
  }

  std::vector<Ref> solve() {
    if (Started) {
      throw std::logic_error(
          "APA sparse elimination is a one-shot construction");
    }
    Started = true;
    const auto N = Base.size();
    const auto AllocStart = Exprs.allocationCount();
    Diagnostics.initial_allocated_nodes = AllocStart - AllocationBaseline;
    Diagnostics.regions = 1;
    std::vector<std::size_t> Ranks;
    {
      ScopedNanoseconds Timer(Diagnostics.metadata_time_ns);
      Ranks = reversePostorderRanks();
    }
    auto Selector = [&]() {
      ScopedNanoseconds Timer(Diagnostics.selection_time_ns);
      return order::OnlineOrderSelector(Policy, Options, std::move(Ranks),
                                        Diagnostics, NodeIds);
    }();
    RecStar.resize(N);
    RecBase.resize(N);
    RecPreds.resize(N);
    QuerySummaries.assign(N, Exprs.zero());
    std::vector<std::size_t> Permutation;
    Permutation.reserve(N);
    measureLive();

    for (std::size_t Step = 0; Step < N; ++Step) {
      const auto V = Selector.chooseNext(
          [&](std::size_t Node) { return signals(Node); },
          [&](std::size_t Node) {
            const auto Self =
                static_cast<std::size_t>(static_cast<bool>(SelfWeight[Node]));
            return Pred[Node].size() + Succ[Node].size() - 2 * Self;
          });
      const auto BeforeAlloc = Exprs.allocationCount();
      const auto BeforeStar = Exprs.starAllocationCount();
      const Ref WStar =
          Exprs.star(SelfWeight[V] ? SelfWeight[V] : Exprs.zero());
      RecStar[V] = WStar;
      RecBase[V] = Base[V];
      Adjacency Predecessors = Pred[V], Successors = Succ[V];
      Predecessors.erase(V);
      Successors.erase(V);
      RecPreds[V] = Predecessors;

      for (const auto &SC : Successors) {
        if (!Factory::isZero(Base[V])) {
          Base[SC.first] = Exprs.unite(
              Base[SC.first], compose(compose(Base[V], WStar), SC.second));
        }
        Selector.markDirty(SC.first);
      }
      std::size_t Bypasses = 0;
      for (const auto &PR : Predecessors) {
        const Ref Left = compose(PR.second, WStar);
        for (const auto &SC : Successors) {
          addEdge(PR.first, SC.first, compose(Left, SC.second));
          ++Bypasses;
        }
        Selector.markDirty(PR.first);
      }
      // Sample transient fill before detaching the pivot as well as the
      // post-detachment graph: otherwise a policy's peak can be understated.
      measureLive();
      for (const auto &PR : Predecessors) {
        Succ[PR.first].erase(V);
      }
      for (const auto &SC : Successors) {
        Pred[SC.first].erase(V);
      }
      Succ[V].clear();
      Pred[V].clear();
      SelfWeight[V].reset();
      Base[V] = Exprs.zero();
      Dead[V] = true;
      Permutation.push_back(V);
      const auto Live = measureLive();
      Diagnostics.bypasses += Bypasses;
      if (Options.RecordTrace) {
        const auto &Sample = Selector.selectedScore(V);
        EliminationStepTrace Trace;
        Trace.step = Step;
        Trace.node = Sample.node;
        Trace.signals = Sample.signals;
        Trace.score = Sample.score;
        Trace.allocated_nodes = Exprs.allocationCount() - BeforeAlloc;
        Trace.allocated_stars = Exprs.starAllocationCount() - BeforeStar;
        Trace.live_nodes = Live.first;
        Trace.live_edges = Live.second;
        Trace.active_nodes = LastActive.first;
        Trace.active_edges = LastActive.second;
        Trace.bypasses = Bypasses;
        Diagnostics.trace.push_back(std::move(Trace));
      }
    }

    const auto BeforeBacksubstitution = Exprs.allocationCount();
    Diagnostics.elimination_allocated_nodes =
        BeforeBacksubstitution - AllocStart;
    for (auto It = Permutation.rbegin(); It != Permutation.rend(); ++It) {
      const auto V = *It;
      Ref Acc = RecBase[V];
      for (const auto &PR : RecPreds[V]) {
        Acc = Exprs.unite(Acc, compose(QuerySummaries[PR.first], PR.second));
      }
      QuerySummaries[V] = compose(Acc, RecStar[V]);
      measureLive();
      RecStar[V].reset();
      RecBase[V].reset();
      RecPreds[V].clear();
      measureLive();
    }
    Diagnostics.backsubstitution_allocated_nodes =
        Exprs.allocationCount() - BeforeBacksubstitution;
    Diagnostics.allocated_nodes = Exprs.allocationCount() - AllocationBaseline;
    Diagnostics.allocated_stars = Exprs.starAllocationCount() - StarBaseline;
    return QuerySummaries;
  }

  const OrderingDiagnostics &diagnostics() const { return Diagnostics; }

private:
  void warmSize(const Ref &Root) {
    const auto Needs = order::requirements(Policy);
    if (Needs.expression_growth || Needs.star_exposure) {
      ScopedNanoseconds Timer(Diagnostics.metadata_time_ns);
      Exprs.cacheReachableNodeCount(Root, Options.DAGSizeCap);
    }
  }
  Ref compose(const Ref &A, const Ref &B) const {
    return Forward ? Exprs.concat(A, B) : Exprs.concat(B, A);
  }

  std::vector<std::size_t> reversePostorderRanks() const {
    const auto N = Base.size();
    // Prefix equations are transposed for solving, not for the input-graph
    // tie-breaker. Keep RPO in the host graph's original edge direction.
    const auto &InputSucc = Forward ? Succ : Pred;
    std::vector<bool> Seen(N, false);
    std::vector<std::size_t> Roots, Postorder;
    for (std::size_t I = 0; I < N; ++I) {
      if (!Factory::isZero(Base[I])) {
        Roots.push_back(I);
      }
    }
    for (std::size_t I = 0; I < N; ++I) {
      Roots.push_back(
          I); // Include disconnected/unreachable vertices deterministically.
    }
    using Frame = std::pair<std::size_t, typename Adjacency::const_iterator>;
    for (auto Root : Roots) {
      if (Seen[Root]) {
        continue;
      }
      Seen[Root] = true;
      std::vector<Frame> Stack{{Root, InputSucc[Root].begin()}};
      while (!Stack.empty()) {
        auto &Top = Stack.back();
        if (Top.second == InputSucc[Top.first].end()) {
          Postorder.push_back(Top.first);
          Stack.pop_back();
          continue;
        }
        const auto Child = (Top.second++)->first;
        if (!Seen[Child]) {
          Seen[Child] = true;
          Stack.emplace_back(Child, InputSucc[Child].begin());
        }
      }
    }
    std::vector<std::size_t> Ranks(N);
    for (std::size_t I = 0; I < N; ++I) {
      Ranks[Postorder[N - 1 - I]] = I;
    }
    return Ranks;
  }

  std::pair<std::size_t, std::size_t> measureLive() {
    if (!Options.MeasureLiveNodes && !Options.RecordTrace) {
      return {0, 0};
    }
    std::vector<Ref> Roots;
    for (std::size_t I = 0; I < Base.size(); ++I) {
      if (!Dead[I] && !Factory::isZero(Base[I])) {
        Roots.push_back(Base[I]);
      }
      for (const auto &Edge : Succ[I]) {
        Roots.push_back(Edge.second);
      }
    }
    LastActive = countDag<TransferT>(Roots);
    Diagnostics.peak_active_nodes =
        std::max(Diagnostics.peak_active_nodes, LastActive.first);
    Diagnostics.peak_active_edges =
        std::max(Diagnostics.peak_active_edges, LastActive.second);
    for (std::size_t I = 0; I < RecStar.size(); ++I) {
      Roots.push_back(RecStar[I]);
      Roots.push_back(RecBase[I]);
      Roots.push_back(QuerySummaries[I]);
      for (const auto &Edge : RecPreds[I])
        Roots.push_back(Edge.second);
    }
    const auto Live = countDag<TransferT>(Roots);
    Diagnostics.peak_live_nodes =
        std::max(Diagnostics.peak_live_nodes, Live.first);
    Diagnostics.peak_live_edges =
        std::max(Diagnostics.peak_live_edges, Live.second);
    return Live;
  }

  const Factory &Exprs;
  std::vector<Ref> Base;
  OrderingPolicy Policy;
  OrderPolicyOptions Options;
  bool Forward;
  std::vector<std::size_t> NodeIds;
  std::vector<Adjacency> Succ, Pred;
  std::vector<Ref> SelfWeight;
  std::vector<bool> Dead;
  OrderingDiagnostics Diagnostics;
  bool Started = false;
  std::size_t AllocationBaseline, StarBaseline;
  std::vector<Ref> RecStar, RecBase, QuerySummaries;
  std::vector<Adjacency> RecPreds;
  std::pair<std::size_t, std::size_t> LastActive{0, 0};
};

} // namespace detail
} // namespace elimination
