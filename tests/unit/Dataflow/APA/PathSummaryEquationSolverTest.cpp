#include "Dataflow/APA/Solver/PathSummaryEquationSolver.h"

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

  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(
      G, elimination::PathSummaryEquationOptions{/*EnableParallel=*/false});
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

  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(
      G, elimination::PathSummaryEquationOptions{/*EnableParallel=*/false});
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

TEST(PathSummaryEquationSolver, ParallelAndSerialLayerSolvesMatch) {
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

  elimination::PathSummaryEquationSolver<std::string, std::string> SerialSolver(
      G, elimination::PathSummaryEquationOptions{/*EnableParallel=*/false});
  auto Serial = SerialSolver.solve();

  elimination::PathSummaryEquationSolver<std::string, std::string>
      ParallelSolver(
          G, elimination::PathSummaryEquationOptions{/*EnableParallel=*/true,
                                                     /*GrainSize=*/1});
  auto Parallel = ParallelSolver.solve();

  for (const std::string &Key :
       {"root", "left", "right", "left_leaf", "right_leaf"}) {
    const auto *SerialExpr = Serial.lookup(Key);
    const auto *ParallelExpr = Parallel.lookup(Key);
    ASSERT_NE(SerialExpr, nullptr);
    ASSERT_NE(ParallelExpr, nullptr);
    EXPECT_EQ(evalLanguage(*SerialExpr, 4, 8),
              evalLanguage(*ParallelExpr, 4, 8))
        << Key;
  }

  EXPECT_TRUE(containsWord(Parallel, "root", "r"));
  EXPECT_TRUE(containsWord(Parallel, "root", "Ll"));
  EXPECT_TRUE(containsWord(Parallel, "root", "Lxa"));
  EXPECT_TRUE(containsWord(Parallel, "root", "Rg"));
  EXPECT_TRUE(containsWord(Parallel, "root", "Ryb"));
  EXPECT_GE(Parallel.diagnostics().max_parallel_layer_width, 2u);
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
  Options.EnableParallel = false;
  Options.Direction = elimination::PathSummaryEquationDirection::ForwardPath;
  elimination::PathSummaryEquationSolver<std::string, std::string> Solver(
      G, Options);
  auto Result = Solver.solve();

  EXPECT_TRUE(containsWord(Result, "entry", "s"));
  EXPECT_TRUE(containsWord(Result, "mid", "sa"));
  EXPECT_TRUE(containsWord(Result, "exit", "sab"));
}
