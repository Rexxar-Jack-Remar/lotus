#include "Solvers/EGraph.h"

#include <gtest/gtest.h>

using namespace lotus::egraph;

TEST(EGraphFeatureTest, RecExprRoundTripsSExpressions) {
  auto expr = RecExpr<SymbolLang>::parse("(f (g a) b)");

  EXPECT_EQ(expr.size(), 4u);
  EXPECT_EQ(expr.toString(), "(f (g a) b)");
}

TEST(EGraphFeatureTest, PatternSearchHandlesRepeatedVariables) {
  EGraph<SymbolLang> egraph;
  Id aa = egraph.addExpr(RecExpr<SymbolLang>::parse("(+ a a)"));
  Id ab = egraph.addExpr(RecExpr<SymbolLang>::parse("(+ a b)"));
  egraph.rebuild();

  Pattern<SymbolLang> same = Pattern<SymbolLang>::parse("(+ ?x ?x)");
  auto matches = same.search(egraph);
  EXPECT_EQ(same.nMatches(egraph), 1u);

  bool found_same = false;
  bool found_mixed = false;
  for (const auto &match : matches) {
    if (egraph.find(match.eclass) == egraph.find(aa)) {
      found_same = true;
    }
    if (egraph.find(match.eclass) == egraph.find(ab)) {
      found_mixed = true;
    }
  }

  EXPECT_TRUE(found_same);
  EXPECT_FALSE(found_mixed);
}

TEST(EGraphFeatureTest, MultiPatternSearchFindsCompatibleClauses) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g b a)"));
  egraph.rebuild();

  MultiPattern<SymbolLang> multi({
      {Var::parse("?f"), Pattern<SymbolLang>::parse("(f ?x ?y)")},
      {Var::parse("?g"), Pattern<SymbolLang>::parse("(g ?x ?y)")},
  });

  auto matches = multi.search(egraph);
  EXPECT_EQ(matches.size(), 1u);
  ASSERT_NE(matches.front().get(Var::parse("?f")), nullptr);
  ASSERT_NE(matches.front().get(Var::parse("?g")), nullptr);
}

TEST(EGraphFeatureTest, MultiPatternRespectsRepeatedBinders) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g b a)"));
  egraph.rebuild();

  MultiPattern<SymbolLang> same_binder({
      {Var::parse("?v"), Pattern<SymbolLang>::parse("(f ?x ?y)")},
      {Var::parse("?v"), Pattern<SymbolLang>::parse("(g ?x ?y)")},
  });

  EXPECT_TRUE(same_binder.search(egraph).empty());
}

TEST(EGraphFeatureTest, DotAndExplanationSurfacesAreAvailable) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.add(SymbolLang::leaf("a"));
  Id b = egraph.add(SymbolLang::leaf("b"));
  egraph.unite(a, b, "manual");
  egraph.rebuild();

  auto explanation = explainEquivalence(egraph, a, b);
  ASSERT_TRUE(explanation.has_value());
  ASSERT_EQ(explanation->steps().size(), 1u);
  EXPECT_EQ(explanation->steps().front().reason, "manual");

  std::string dot = toDot(egraph);
  EXPECT_NE(dot.find("digraph egraph"), std::string::npos);
  EXPECT_STREQ(version(), "lotus-egraph-egg-port");
}

TEST(EGraphFeatureTest, ExplanationUnavailableWithoutRecordedPath) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.add(SymbolLang::leaf("a"));
  Id b = egraph.add(SymbolLang::leaf("b"));
  egraph.unite(a, b, "manual");
  egraph.rebuild();

  EXPECT_FALSE(explainEquivalence(egraph, a, b).has_value());
}

namespace {

struct NoCycleAnalysis : NoAnalysis<SymbolLang> {
  bool allowEMatchingCycles() const { return false; }
};

} // namespace

TEST(EGraphFeatureTest, RewriteRespectsCyclePolicy) {
  EGraph<SymbolLang, NoCycleAnalysis> egraph;
  Id a = egraph.add(SymbolLang::leaf("a"));
  egraph.rebuild();

  auto cycle = makeRewrite<SymbolLang, NoCycleAnalysis>("self-wrap", "?x", "(f ?x)");
  auto matches = cycle.search(egraph);
  auto applied = cycle.apply(egraph, matches);
  egraph.rebuild();

  EXPECT_TRUE(applied.empty());
  EXPECT_EQ(egraph.totalSize(), 1u);
}

TEST(EGraphFeatureTest, RunnerTreatsUnionOnlyRewriteAsProgress) {
  auto rule = makeRewrite<SymbolLang>("merge-a-b", "a", "b");

  Runner<SymbolLang> runner;
  runner.withExpr(RecExpr<SymbolLang>::parse("a"))
      .withExpr(RecExpr<SymbolLang>::parse("b"))
      .withIterLimit(4)
      .run({rule});

  ASSERT_FALSE(runner.iterations.empty());
  EXPECT_EQ(runner.stop_reason.kind, StopReasonKind::Saturated);
  EXPECT_EQ(runner.egraph.numberOfClasses(), 1u);
}
