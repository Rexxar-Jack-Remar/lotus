#include "Dataflow/APA/Solver/Elimination/SparseSolver.h"

#include <limits>
#include <random>
#include <set>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

namespace {

using elimination::OrderingPolicy;
using elimination::OrderPolicyOptions;
using elimination::OrderSignals;
using Factory = elimination::PathExprFactory<std::string>;
using SparseSolver = elimination::detail::SparseElimination<std::string>;
using Language = std::set<std::string>;

const std::vector<OrderingPolicy> POLICIES = {
    OrderingPolicy::Structural, OrderingPolicy::ExpressionAware,
    OrderingPolicy::StarRisk, OrderingPolicy::Hybrid};

Language concat(const Language &A, const Language &B, std::size_t Bound) {
  Language Result;
  for (const auto &L : A) {
    for (const auto &R : B) {
      if (L.size() + R.size() <= Bound) {
        Result.insert(L + R);
      }
    }
  }
  return Result;
}

// Exact interpretation in the finite quantale of words of length <= Bound.
// Unlike a fixed unfolding depth, this compares every bounded loop word.
class LanguageInterpreter final {
public:
  explicit LanguageInterpreter(std::size_t Bound) : Bound(Bound) {}
  Language eval(const Factory::Ref &Root) {
    auto Found = Memo.find(Root.get());
    if (Found != Memo.end()) {
      return Found->second;
    }
    Language Result;
    switch (Root->K) {
    case Factory::Kind::Zero:
      break;
    case Factory::Kind::One:
      Result.insert("");
      break;
    case Factory::Kind::Atom:
      if (Root->Transfer->size() <= Bound) {
        Result.insert(*Root->Transfer);
      }
      break;
    case Factory::Kind::Union: {
      Result = eval(Root->L);
      auto Other = eval(Root->R);
      Result.insert(Other.begin(), Other.end());
      break;
    }
    case Factory::Kind::Concat:
      Result = concat(eval(Root->L), eval(Root->R), Bound);
      break;
    case Factory::Kind::Star: {
      const auto Body = eval(Root->L);
      Result.insert("");
      for (;;) {
        const auto Next = concat(Result, Body, Bound);
        const auto OldSize = Result.size();
        Result.insert(Next.begin(), Next.end());
        if (Result.size() == OldSize) {
          break;
        }
      }
      break;
    }
    }
    Memo.emplace(Root.get(), Result);
    return Result;
  }

private:
  std::size_t Bound;
  std::unordered_map<const Factory::Expr *, Language> Memo;
};

struct Edge {
  std::size_t source, target;
  std::string label;
};

Language enumeratePaths(std::size_t N, const std::vector<Edge> &Edges,
                        std::size_t Target, std::size_t Bound) {
  std::vector<Language> At(N);
  At[0].insert("");
  for (;;) {
    bool Changed = false;
    for (const auto &Edge : Edges) {
      const auto Incoming =
          concat(At[Edge.source], Language{Edge.label}, Bound);
      const auto Before = At[Edge.target].size();
      At[Edge.target].insert(Incoming.begin(), Incoming.end());
      Changed |= Before != At[Edge.target].size();
    }
    if (!Changed) {
      return At[Target];
    }
  }
}

std::vector<std::size_t> pivots(const elimination::OrderingDiagnostics &D) {
  std::vector<std::size_t> Result;
  for (const auto &Step : D.trace) {
    Result.push_back(Step.node);
  }
  return Result;
}

} // namespace

TEST(APAOnlineOrder, WorkedPolicyScoresMatchDraft) {
  OrderPolicyOptions Opts;
  Opts.StructuralCap = 12;
  Opts.ExpressionCap = 48;
  Opts.StarCap = 3;
  const OrderSignals A{12, 48, 0}, B{4, 48, 0}, C{4, 28, 3};
  EXPECT_DOUBLE_EQ(
      elimination::order::policyScore(OrderingPolicy::Structural, A, Opts), 12);
  EXPECT_DOUBLE_EQ(
      elimination::order::policyScore(OrderingPolicy::ExpressionAware, C, Opts),
      28);
  EXPECT_DOUBLE_EQ(
      elimination::order::policyScore(OrderingPolicy::StarRisk, A, Opts), 1);
  EXPECT_DOUBLE_EQ(
      elimination::order::policyScore(OrderingPolicy::StarRisk, B, Opts),
      1.0 / 3);
  EXPECT_DOUBLE_EQ(
      elimination::order::policyScore(OrderingPolicy::Hybrid, B, Opts),
      4.0 / 3);
  EXPECT_NEAR(elimination::order::policyScore(OrderingPolicy::Hybrid, C, Opts),
              1.0 / 3 + 28.0 / 48 + 1, 1e-12);
  const std::vector<OrderSignals> Signals{A, B, C};
  const std::vector<std::size_t> Expected{1, 2, 1, 1};
  for (std::size_t I = 0; I < POLICIES.size(); ++I) {
    elimination::OrderingDiagnostics Diag;
    elimination::order::OnlineOrderSelector Selector(POLICIES[I], Opts,
                                                     {2, 0, 1}, Diag);
    EXPECT_EQ(Selector.chooseNext([&](std::size_t V) { return Signals[V]; },
                                  [](std::size_t) { return 0; }),
              Expected[I]);
  }
}

TEST(APAOnlineOrder, ScoresAlwaysUseAllTermsOfTheRequestedPolicy) {
  OrderPolicyOptions Opts;
  const OrderSignals Signal{10000, 90000, 500};
  EXPECT_DOUBLE_EQ(elimination::order::policyScore(
                       OrderingPolicy::ExpressionAware, Signal, Opts),
                   Signal.expression);
  EXPECT_DOUBLE_EQ(
      elimination::order::policyScore(OrderingPolicy::StarRisk, Signal, Opts),
      2.0);
  EXPECT_DOUBLE_EQ(
      elimination::order::policyScore(OrderingPolicy::Hybrid, Signal, Opts),
      3.0);
}

TEST(APAOnlineOrder, DAGSizeCacheCountsDistinctNodesAndHonorsCaps) {
  Factory E;
  const auto A = E.atom("a"), B = E.atom("b"), C = E.atom("c");
  const auto Root = E.unite(E.concat(A, B), E.concat(A, C));
  EXPECT_FALSE(E.cachedReachableNodeCount(Root).has_value());
  EXPECT_EQ(E.cacheReachableNodeCount(Root), 6u);
  EXPECT_EQ(E.cachedReachableNodeCount(Root).value(), 6u);
  EXPECT_EQ(E.cacheReachableNodeCount(Root, 3), 3u);
  EXPECT_EQ(E.cacheReachableNodeCount(E.concat(A, A)), 2u);
  EXPECT_EQ(E.cacheReachableNodeCount(E.zero()), 0u);
  EXPECT_EQ(E.cacheReachableNodeCount(E.one()), 1u);
  const auto Before = E.allocationCount();
  E.atom("a");
  E.concat(A, B);
  EXPECT_EQ(E.allocationCount(), Before);
}

TEST(APAOnlineOrder, LocalSignalsMatchDraftAndCacheIsSemanticNotSyntactic) {
  Factory E;
  const auto Small = E.atom("x");
  const auto Large = E.concat(E.atom("a"), E.concat(E.atom("b"), E.atom("c")));
  const auto Loop = E.atom("d");
  const auto Self = E.unite(E.one(), Loop); // Three nodes in M[k,k].
  E.star(Self); // Syntactic hash-consing must NOT zero semantic exposure.
  auto Build = [&](SparseSolver &Solver) {
    for (std::size_t I = 3; I < 6; ++I)
      Solver.addEdge(I, 0, Small);
    for (std::size_t I = 6; I < 10; ++I)
      Solver.addEdge(0, I, Small);
    for (std::size_t I = 10; I < 12; ++I)
      Solver.addEdge(I, 1, Large);
    for (std::size_t I = 12; I < 14; ++I)
      Solver.addEdge(1, I, Large);
    for (std::size_t I = 14; I < 16; ++I)
      Solver.addEdge(I, 2, Small);
    for (std::size_t I = 16; I < 18; ++I)
      Solver.addEdge(2, I, Small);
    Solver.addEdge(2, 2, Loop);
  };
  SparseSolver Solver(E, std::vector<Factory::Ref>(18, E.zero()),
                      OrderingPolicy::Hybrid);
  Build(Solver);
  EXPECT_DOUBLE_EQ(Solver.signals(0).structural, 12);
  EXPECT_DOUBLE_EQ(Solver.signals(0).expression, 48);
  EXPECT_DOUBLE_EQ(Solver.signals(1).expression, 48);
  EXPECT_DOUBLE_EQ(Solver.signals(2).expression, 28);
  EXPECT_DOUBLE_EQ(Solver.signals(2).star, 3);
  OrderPolicyOptions Opts;
  Opts.IsStarResultCached = [&](const void *Operand) {
    return Operand == Self.get();
  };
  SparseSolver Cached(E, std::vector<Factory::Ref>(18, E.zero()),
                      OrderingPolicy::Hybrid, Opts);
  Build(Cached);
  EXPECT_DOUBLE_EQ(Cached.signals(2).star, 0);
}

TEST(APAOnlineOrder, VersionedHeapDiscardsObsoleteScores) {
  OrderPolicyOptions Opts;
  elimination::OrderingDiagnostics D;
  elimination::order::OnlineOrderSelector Selector(OrderingPolicy::Structural,
                                                   Opts, {0, 1, 2}, D);
  std::vector<OrderSignals> Signals{{2, 0, 0}, {1, 0, 0}, {3, 0, 0}};
  auto Read = [&](std::size_t V) { return Signals[V]; };
  auto Degree = [](std::size_t) { return 0; };
  EXPECT_EQ(Selector.chooseNext(Read, Degree), 1u);
  Signals[0].structural = 10;
  Selector.markDirty(0);
  EXPECT_EQ(Selector.chooseNext(Read, Degree), 2u);
  EXPECT_EQ(Selector.chooseNext(Read, Degree), 0u);
  EXPECT_EQ(D.stale_heap_entries, 1u);
  EXPECT_EQ(D.score_refreshes, 4u);
}

TEST(APAOnlineOrder,
     RandomizedIncrementalMatchesFullRescoringAndPathEnumeration) {
  std::mt19937 Generator(27);
  for (std::size_t Trial = 0; Trial < 30; ++Trial) {
    constexpr std::size_t N = 5;
    std::vector<Edge> Edges;
    for (std::size_t I = 0; I < N; ++I) {
      for (std::size_t J = 0; J < N; ++J) {
        if (Generator() % 4 == 0) {
          Edges.push_back({I, J, Generator() % 2 ? "a" : "b"});
        }
      }
    }
    for (auto Policy : POLICIES) {
      Factory E;
      std::vector<Factory::Ref> Base(N, E.zero());
      Base[0] = E.one();
      OrderPolicyOptions Opts;
      Opts.RecordTrace = true;
      SparseSolver Incremental(E, Base, Policy, Opts);
      Opts.Incremental = false;
      SparseSolver Full(E, Base, Policy, Opts);
      for (const auto &Edge : Edges) {
        Incremental.addEdge(Edge.source, Edge.target, E.atom(Edge.label));
        Full.addEdge(Edge.source, Edge.target, E.atom(Edge.label));
      }
      const auto A = Incremental.solve(), B = Full.solve();
      const auto &DA = Incremental.diagnostics(), &DB = Full.diagnostics();
      EXPECT_EQ(pivots(DA), pivots(DB)) << "trial " << Trial;
      EXPECT_LE(DA.score_refreshes, DB.score_refreshes);
      EXPECT_EQ(DA.trace.size(), N);
      LanguageInterpreter Interpreter(4);
      for (std::size_t I = 0; I < N; ++I) {
        EXPECT_DOUBLE_EQ(DA.trace[I].score, DB.trace[I].score);
        EXPECT_EQ(Interpreter.eval(A[I]), Interpreter.eval(B[I]));
        EXPECT_EQ(Interpreter.eval(A[I]), enumeratePaths(N, Edges, I, 4))
            << "trial " << Trial << ", node " << I;
      }
      EXPECT_EQ(DA.trace.back().active_nodes, 0u);
      EXPECT_GT(DA.peak_live_nodes, 0u);
    }
  }
}

TEST(APAOnlineOrder,
     ExhaustiveOrdersProvideSmallGraphAllocationAndLiveDAGOracle) {
  const std::vector<Edge> Edges{{0, 1, "a"}, {0, 2, "b"}, {1, 2, "a"},
                                {2, 1, "b"}, {1, 3, "b"}, {2, 3, "a"}};
  std::vector<std::size_t> Permutation{0, 1, 2, 3};
  std::size_t BestAlloc = std::numeric_limits<std::size_t>::max();
  std::size_t BestLive = BestAlloc;
  auto Run = [&](OrderingPolicy Policy, OrderPolicyOptions Opts) {
    Factory E;
    Opts.MeasureLiveNodes = true;
    SparseSolver Solver(E, {E.one(), E.zero(), E.zero(), E.zero()}, Policy,
                        Opts);
    for (const auto &Edge : Edges)
      Solver.addEdge(Edge.source, Edge.target, E.atom(Edge.label));
    const auto Result = Solver.solve();
    LanguageInterpreter Interpreter(4);
    for (std::size_t I = 0; I < 4; ++I) {
      EXPECT_EQ(Interpreter.eval(Result[I]), enumeratePaths(4, Edges, I, 4));
    }
    return Solver.diagnostics();
  };
  do {
    OrderPolicyOptions Opts;
    Opts.ExplicitOrder = Permutation;
    const auto D = Run(OrderingPolicy::Explicit, Opts);
    BestAlloc = std::min(BestAlloc, D.allocated_nodes);
    BestLive = std::min(BestLive, D.peak_live_nodes);
  } while (std::next_permutation(Permutation.begin(), Permutation.end()));
  EXPECT_GT(BestAlloc, 0u);
  EXPECT_GT(BestLive, 0u);
  for (auto Policy : POLICIES) {
    const auto D = Run(Policy, {});
    EXPECT_GE(D.allocated_nodes, BestAlloc);
    EXPECT_GE(D.peak_live_nodes, BestLive);
  }
}

TEST(APAOnlineOrder, RejectsInvalidCapsAndExplicitPermutations) {
  Factory E;
  OrderPolicyOptions Opts;
  Opts.StructuralCap = 0;
  EXPECT_THROW(SparseSolver(E, {E.one()}, OrderingPolicy::Hybrid, Opts),
               std::invalid_argument);
  Opts.StructuralCap = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(SparseSolver(E, {E.one()}, OrderingPolicy::Hybrid, Opts),
               std::invalid_argument);
  Opts.StructuralCap = 64;
  Opts.ExplicitOrder = {0, 0};
  EXPECT_THROW(
      SparseSolver(E, {E.one(), E.zero()}, OrderingPolicy::Explicit, Opts),
      std::invalid_argument);
}

TEST(APAOnlineOrder, SeededRandomOrderingIsReproducible) {
  auto Run = [](std::uint64_t Seed) {
    Factory E;
    OrderPolicyOptions Opts;
    Opts.RecordTrace = true;
    Opts.RandomSeed = Seed;
    SparseSolver Solver(E, std::vector<Factory::Ref>(10, E.one()),
                        OrderingPolicy::Random, Opts);
    Solver.solve();
    return pivots(Solver.diagnostics());
  };
  EXPECT_EQ(Run(42), Run(42));
  EXPECT_NE(Run(42), Run(99));
}

TEST(APAOnlineOrder, StarRiskIsComputedWithoutASemanticCacheProvider) {
  Factory E;
  OrderPolicyOptions Opts;
  Opts.RecordTrace = true;
  const auto Loop = E.atom("step");
  SparseSolver Solver(E, {E.one()}, OrderingPolicy::StarRisk, Opts);
  Solver.addEdge(0, 0, Loop);
  EXPECT_DOUBLE_EQ(Solver.signals(0).star, 3.0);
  EXPECT_DOUBLE_EQ(elimination::order::policyScore(OrderingPolicy::StarRisk,
                                                   Solver.signals(0), Opts),
                   3.0 / Opts.StarCap);
}

TEST(APAOnlineOrder, EveryAllocationPhaseIsCountedAndQueryRootsAreLive) {
  Factory E;
  OrderPolicyOptions Opts;
  Opts.RecordTrace = true;
  SparseSolver Solver(E, {E.one(), E.zero(), E.zero()}, OrderingPolicy::Hybrid,
                      Opts);
  Solver.addEdge(0, 1, E.atom("a"));
  Solver.addEdge(1, 1, E.atom("b"));
  Solver.addEdge(1, 2, E.atom("a"));
  const auto Results = Solver.solve();
  const auto &D = Solver.diagnostics();
  EXPECT_EQ(D.allocated_nodes, D.initial_allocated_nodes +
                                   D.elimination_allocated_nodes +
                                   D.backsubstitution_allocated_nodes);
  EXPECT_GT(D.initial_allocated_nodes, 0u);
  EXPECT_EQ(D.trace.back().active_nodes, 0u);
  EXPECT_GT(D.trace.back().live_nodes, 0u);
  const auto Final = elimination::detail::countDag<std::string>(Results);
  EXPECT_GE(D.peak_live_nodes, Final.first);
}

TEST(APAOnlineOrder, HeapCompactionPreservesCurrentMinima) {
  constexpr std::size_t N = 100;
  std::vector<std::size_t> Ranks(N);
  std::iota(Ranks.begin(), Ranks.end(), 0);
  OrderPolicyOptions Opts;
  elimination::OrderingDiagnostics D;
  elimination::order::OnlineOrderSelector Selector(OrderingPolicy::Structural,
                                                   Opts, Ranks, D);
  for (std::size_t Step = 0; Step < N; ++Step) {
    EXPECT_EQ(Selector.chooseNext(
                  [&](std::size_t) {
                    return OrderSignals{static_cast<double>(N - Step), 0, 0};
                  },
                  [](std::size_t) { return 0; }),
              Step);
    for (std::size_t I = Step + 1; I < N; ++I)
      Selector.markDirty(I);
  }
  EXPECT_GT(D.heap_compactions, 0u);
}
