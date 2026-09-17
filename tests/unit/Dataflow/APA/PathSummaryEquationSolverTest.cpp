#include "Dataflow/APA/Solver/Equations/Solver.h"

#include <set>
#include <string>

#include <gtest/gtest.h>

namespace {

using Graph = elimination::PathSummaryEquationGraph<std::string, std::string>;
using ExprRef = Graph::expr_ref_t;
using ExprFactory = Graph::expr_factory_t;

std::set<std::string> concatLanguage(const std::set<std::string> &Lhs,
                                     const std::set<std::string> &Rhs,
                                     std::size_t MaxLen) {
  std::set<std::string> Out;
  for (const auto &L : Lhs) {
    for (const auto &R : Rhs) {
      auto Combined = L + R;
      if (Combined.size() <= MaxLen) {
        Out.insert(std::move(Combined));
      }
    }
  }
  return Out;
}

std::set<std::string> evalLanguage(const ExprRef &Expr, std::size_t StarDepth,
                                   std::size_t MaxLen) {
  if (!Expr) {
    return {};
  }

  using Kind = ExprFactory::Kind;
  switch (Expr->K) {
  case Kind::Zero:
    return {};
  case Kind::One:
    return {""};
  case Kind::Atom:
    return {*Expr->Transfer};
  case Kind::Union: {
    auto Lhs = evalLanguage(Expr->L, StarDepth, MaxLen);
    auto Rhs = evalLanguage(Expr->R, StarDepth, MaxLen);
    Lhs.insert(Rhs.begin(), Rhs.end());
    return Lhs;
  }
  case Kind::Concat:
    return concatLanguage(evalLanguage(Expr->L, StarDepth, MaxLen),
                          evalLanguage(Expr->R, StarDepth, MaxLen), MaxLen);
  case Kind::Star: {
    auto Base = evalLanguage(Expr->L, StarDepth, MaxLen);
    std::set<std::string> Result{""};
    std::set<std::string> Current{""};
    for (std::size_t I = 0; I < StarDepth; ++I) {
      Current = concatLanguage(Current, Base, MaxLen);
      Result.insert(Current.begin(), Current.end());
    }
    return Result;
  }
  }
  return {};
}

bool containsWord(
    const elimination::PathSummaryEquationResult<std::string, std::string>
        &Result,
    const std::string &Key, const std::string &Word) {
  const auto *Expr = Result.lookup(Key);
  if (!Expr) {
    return false;
  }
  auto Language = evalLanguage(*Expr, 4, 8);
  return Language.count(Word) != 0;
}

} // namespace

TEST(PathSummaryEquationSolver, SolvesAcyclicSummaryDependencies) {
  Graph G;
  auto &E = G.exprs();
  G.addNode("main", E.atom("m"));
  G.addNode("helper", E.atom("h"));
  G.addNode("leaf", E.atom("l"));
  G.addEdge("main", "helper", E.atom("p"));
  G.addEdge("helper", "leaf", E.atom("q"));

  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(G);
  auto Result = Solver.solve();

  EXPECT_TRUE(containsWord(Result, "main", "m"));
  EXPECT_TRUE(containsWord(Result, "main", "ph"));
  EXPECT_TRUE(containsWord(Result, "main", "pql"));
  EXPECT_TRUE(containsWord(Result, "helper", "h"));
  EXPECT_TRUE(containsWord(Result, "helper", "ql"));
  EXPECT_EQ(Result.diagnostics().scc_count, 3u);
  EXPECT_EQ(Result.diagnostics().cyclic_scc_count, 0u);
}

TEST(PathSummaryEquationSolver, ClosesRecursiveSummarySCCWithStar) {
  Graph G;
  auto &E = G.exprs();
  G.addNode("A", E.atom("a"));
  G.addNode("B", E.atom("b"));
  G.addEdge("A", "B", E.atom("x"));
  G.addEdge("B", "A", E.atom("y"));

  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(G);
  auto Result = Solver.solve();

  EXPECT_TRUE(containsWord(Result, "A", "a"));
  EXPECT_TRUE(containsWord(Result, "A", "xb"));
  EXPECT_TRUE(containsWord(Result, "A", "xya"));
  EXPECT_TRUE(containsWord(Result, "A", "xyxb"));
  EXPECT_TRUE(containsWord(Result, "B", "b"));
  EXPECT_TRUE(containsWord(Result, "B", "ya"));
  EXPECT_TRUE(containsWord(Result, "B", "yxb"));
  EXPECT_EQ(Result.diagnostics().scc_count, 1u);
  EXPECT_EQ(Result.diagnostics().cyclic_scc_count, 1u);
}

TEST(PathSummaryEquationSolver, SolvesIndependentDependencyBranches) {
  Graph G;
  auto &E = G.exprs();
  G.addNode("root", E.atom("r"));
  G.addNode("left", E.atom("l"));
  G.addNode("right", E.atom("g"));
  G.addNode("left_leaf", E.atom("a"));
  G.addNode("right_leaf", E.atom("b"));
  G.addEdge("root", "left", E.atom("L"));
  G.addEdge("root", "right", E.atom("R"));
  G.addEdge("left", "left_leaf", E.atom("x"));
  G.addEdge("right", "right_leaf", E.atom("y"));

  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(G);
  auto Result = Solver.solve();

  EXPECT_TRUE(containsWord(Result, "root", "r"));
  EXPECT_TRUE(containsWord(Result, "root", "Ll"));
  EXPECT_TRUE(containsWord(Result, "root", "Lxa"));
  EXPECT_TRUE(containsWord(Result, "root", "Rg"));
  EXPECT_TRUE(containsWord(Result, "root", "Ryb"));
}

TEST(PathSummaryEquationSolver, ForwardPathDirectionComposesAfterSource) {
  Graph G;
  auto &E = G.exprs();
  G.addNode("entry", E.atom("s"));
  G.addNode("mid");
  G.addNode("exit");
  G.addEdge("entry", "mid", E.atom("a"));
  G.addEdge("mid", "exit", E.atom("b"));

  elimination::PathSummaryEquationOptions Options;
  Options.Direction = elimination::PathSummaryEquationDirection::ForwardPath;
  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(
      G, Options);
  auto Result = Solver.solve();

  EXPECT_TRUE(containsWord(Result, "entry", "s"));
  EXPECT_TRUE(containsWord(Result, "mid", "sa"));
  EXPECT_TRUE(containsWord(Result, "exit", "sab"));
}

TEST(PathSummaryEquationSolver,
     OnlinePoliciesPreserveBothCompositionDirections) {
  for (auto Direction :
       {elimination::PathSummaryEquationDirection::ForwardPath,
        elimination::PathSummaryEquationDirection::DependencyPrefix}) {
    Graph G;
    auto &E = G.exprs();
    G.addNode("A", E.atom("a"));
    G.addNode("B", E.atom("b"));
    G.addNode("C", E.atom("c"));
    G.addEdge("A", "B", E.atom("x"));
    G.addEdge("B", "A", E.atom("y"));
    G.addEdge("B", "C", E.atom("z"));
    elimination::PathSummaryEquationOptions Opts;
    Opts.Direction = Direction;
    elimination::PathSummaryEquationSolver<std::string, std::string> Baseline(
        G, Opts);
    auto Reference = Baseline.solve();
    for (auto Policy : {elimination::OrderingPolicy::Structural,
                        elimination::OrderingPolicy::ExpressionAware,
                        elimination::OrderingPolicy::StarRisk,
                        elimination::OrderingPolicy::Hybrid,
                        elimination::OrderingPolicy::ReversePostOrder,
                        elimination::OrderingPolicy::Random,
                        elimination::OrderingPolicy::MinDegree}) {
      Opts.Ordering = Policy;
      Opts.Order.RecordTrace = true;
      elimination::PathSummaryEquationSolver<std::string, std::string> Solver(
          G, Opts);
      const auto Result = Solver.solve();
      for (const auto &Key : {"A", "B", "C"}) {
        ASSERT_NE(Result.lookup(Key), nullptr);
        EXPECT_EQ(evalLanguage(*Result.lookup(Key), 6, 6),
                  evalLanguage(*Reference.lookup(Key), 6, 6))
            << Key;
      }
      EXPECT_EQ(Result.diagnostics().ordering.trace.size(), 2u);
    }
  }
}

TEST(PathSummaryEquationSolver, AggregatesRegionsAndKeepsGraphNodeIdentifiers) {
  Graph G;
  auto &E = G.exprs();
  G.addNode("A", E.one());
  G.addNode("B");
  G.addNode("C");
  G.addEdge("A", "A", E.atom("a"));
  G.addEdge("A", "B", E.atom("b"));
  G.addEdge("B", "C", E.atom("c"));
  G.addEdge("C", "B", E.atom("d"));
  elimination::PathSummaryEquationOptions Opts;
  Opts.Direction = elimination::PathSummaryEquationDirection::ForwardPath;
  Opts.Ordering = elimination::OrderingPolicy::Hybrid;
  Opts.Order.RecordTrace = true;
  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(G,
                                                                          Opts);
  const auto R = Solver.solve();
  const auto &D = R.diagnostics().ordering;
  EXPECT_EQ(D.regions, 2u);
  ASSERT_EQ(D.trace.size(), 3u);
  std::set<std::size_t> Seen;
  for (const auto &Step : D.trace)
    Seen.insert(Step.node);
  EXPECT_EQ(Seen, (std::set<std::size_t>{0, 1, 2}));
  EXPECT_NE(D.trace.front().region, D.trace.back().region);
}

// Large cyclic SCC (> kDenseCyclicThreshold) forces the sparse min-fill
// Gaussian-elimination path.  A 20-node ring n0->n1->...->n19->n0 with a base
// word at n0 must still yield the correct forward languages, matching what the
// dense Floyd--Warshall closure would produce on a small ring.
TEST(PathSummaryEquationSolver, LargeCyclicSCCSparsePathMatchesLanguage) {
  Graph G;
  auto &E = G.exprs();
  const std::size_t N = 20; // > 16, so the solver takes the sparse branch
  auto name = [](std::size_t I) { return "n" + std::to_string(I); };
  // Base word "s" enters the ring at n0; every other node starts empty.
  G.addNode(name(0), E.atom("s"));
  for (std::size_t I = 1; I < N; ++I) {
    G.addNode(name(I));
  }
  // Ring edges labelled with single distinct letters a, b, c, ...
  for (std::size_t I = 0; I < N; ++I) {
    std::string Lbl(1, static_cast<char>('a' + static_cast<int>(I)));
    G.addEdge(name(I), name((I + 1) % N), E.atom(Lbl));
  }

  elimination::PathSummaryEquationOptions Options;
  Options.Direction = elimination::PathSummaryEquationDirection::ForwardPath;
  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(
      G, Options);
  auto Result = Solver.solve();

  // One strongly-connected component that is cyclic (confirms the sparse path
  // exercised the cyclic branch, not the acyclic singleton one).
  EXPECT_EQ(Result.diagnostics().scc_count, 1u);
  EXPECT_EQ(Result.diagnostics().cyclic_scc_count, 1u);

  // Forward reachability from the base at n0: X_{nk} contains s followed by the
  // edge labels a,b,c,... up to nk (within the length-8 language bound).
  EXPECT_TRUE(containsWord(Result, "n0", "s"));
  EXPECT_TRUE(containsWord(Result, "n1", "sa"));
  EXPECT_TRUE(containsWord(Result, "n2", "sab"));
  EXPECT_TRUE(containsWord(Result, "n3", "sabc"));
  EXPECT_TRUE(containsWord(Result, "n5", "sabcde"));
}
