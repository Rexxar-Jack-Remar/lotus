#include "Solvers/EGraph.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

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
  const auto &flat = explanation->makeFlatExplanation();
  ASSERT_EQ(flat.size(), 2u);
  EXPECT_EQ(flat.front().forward_rule, std::nullopt);
  EXPECT_EQ(flat.front().expr.toString(), "a");
  EXPECT_EQ(flat.back().forward_rule, std::optional<Symbol>(Symbol("manual")));
  EXPECT_EQ(flat.back().expr.toString(), "b");

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
  egraph.add(SymbolLang::leaf("a"));
  egraph.rebuild();

  auto cycle =
      makeRewrite<SymbolLang, NoCycleAnalysis>("self-wrap", "?x", "(f ?x)");
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

TEST(EGraphFeatureTest, PatternSearchWithLimitUsesOperatorIndex) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(+ a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(+ a c)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(* a b)"));
  egraph.rebuild();

  auto pat = Pattern<SymbolLang>::parse("(+ ?x ?y)");
  auto matches = pat.searchWithLimit(egraph, 1);

  ASSERT_EQ(matches.size(), 1u);
  ASSERT_EQ(matches.front().substs.size(), 1u);
}

TEST(EGraphFeatureTest, MultiPatternParsesAndApplies) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a b)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(g a b)"));
  egraph.rebuild();

  auto mp = MultiPattern<SymbolLang>::parse("?u = (f ?x ?y), ?v = (g ?x ?y)");
  auto matches = mp.search(egraph);
  ASSERT_EQ(matches.size(), 1u);

  auto added = mp.apply(egraph, matches);
  egraph.rebuild();
  EXPECT_EQ(added.size(), 1u);
}

TEST(EGraphFeatureTest, ExplanationSupportsExpressionQueries) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  egraph.rebuild();
  egraph.unite(a, b, "manual");
  egraph.rebuild();

  auto explanation = explainEquivalence(egraph, RecExpr<SymbolLang>::parse("a"),
                                        RecExpr<SymbolLang>::parse("b"));
  ASSERT_TRUE(explanation.has_value());
  EXPECT_FALSE(explanation->getFlatStrings().empty());
  EXPECT_FALSE(explanation->explanationTrees().empty());
  ASSERT_GE(explanation->makeFlatExplanation().size(), 2u);
  EXPECT_EQ(explanation->makeFlatExplanation().back().forward_rule,
            std::optional<Symbol>(Symbol("manual")));
}

TEST(EGraphFeatureTest, EquivsReturnsMatchingEClasses) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.rebuild();

  auto matches = egraph.equivs(RecExpr<SymbolLang>::parse("(f a)"),
                               RecExpr<SymbolLang>::parse("(f a)"));
  EXPECT_FALSE(matches.empty());
  EXPECT_TRUE(egraph.equivalent(RecExpr<SymbolLang>::parse("(f a)"),
                                RecExpr<SymbolLang>::parse("(f a)")));
}

TEST(EGraphFeatureTest, PatternAlphaRenameCanonicalizesVariables) {
  auto pat = Pattern<SymbolLang>::parse("(f ?a ?b ?a)");
  auto renamed = pat.alphaRename();
  EXPECT_EQ(renamed.toString(), "(f ?x ?y ?x)");
}

TEST(EGraphFeatureTest, NumericVarsAndSubstInsertBehaveLikeEgg) {
  Var v = Var::parse("?#0012");
  ASSERT_TRUE(v.asU32().has_value());
  EXPECT_EQ(*v.asU32(), 12u);

  Subst subst = Subst::withCapacity(2);
  EXPECT_FALSE(subst.insert(v, Id::fromIndex(1)).has_value());
  auto old = subst.insert(v, Id::fromIndex(2));
  ASSERT_TRUE(old.has_value());
  EXPECT_EQ(old->value(), 1u);
}

TEST(EGraphFeatureTest, DotWrapperCanSerializeToFile) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.rebuild();

  auto path = std::filesystem::temp_directory_path() / "lotus_egraph_test.dot";
  Dot<SymbolLang, NoAnalysis<SymbolLang>>(egraph).toDot(path.string());
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("digraph egraph"), std::string::npos);
  std::filesystem::remove(path);
}

TEST(EGraphFeatureTest, PatternProgramRunsWithLimit) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f b)"));
  egraph.rebuild();

  auto pattern = Pattern<SymbolLang>::parse("(f ?x)");
  auto program = PatternProgram<SymbolLang>::compileFromPattern(pattern);
  auto ids = egraph.classIds();
  auto substs = program.runWithLimit(egraph, ids.front(), 1);
  EXPECT_LE(substs.size(), 1u);
}

TEST(EGraphFeatureTest, ExplanationOptimizationFlagRequiresExplanations) {
  EGraph<SymbolLang> egraph;
  EXPECT_THROW((void)egraph.withoutExplanationLengthOptimization(),
               std::runtime_error);
  EXPECT_THROW((void)egraph.withExplanationLengthOptimization(),
               std::runtime_error);

  auto explained =
      egraph.withExplanationsEnabled().withoutExplanationLengthOptimization();
  EXPECT_FALSE(explained.optimizeExplanationLengths());
  EXPECT_TRUE(explained.withExplanationLengthOptimization()
                  .optimizeExplanationLengths());
}

TEST(EGraphFeatureTest, DotRunMatchesEggBehaviorWhenGraphvizIsAvailable) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.rebuild();

  Dot<SymbolLang, NoAnalysis<SymbolLang>> dot(egraph);
  if (std::system("command -v dot >/dev/null 2>&1") != 0) {
    GTEST_SKIP() << "Graphviz 'dot' not available";
  }

  EXPECT_NO_THROW(dot.runDot(std::vector<std::string>{"-Tsvg"}));
  EXPECT_NO_THROW(dot.run(std::string("dot"), std::vector<std::string>{"-Tsvg"}));
}

TEST(EGraphFeatureTest, LpExtractorTimeoutValidationAndCanonicalRoots) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  egraph.unite(a, b, "manual");
  egraph.rebuild();

  LpExtractor<SymbolLang> extractor(egraph);
  auto result = extractor.solveMultiple({a, b});
  ASSERT_EQ(result.second.size(), 2u);
  EXPECT_EQ(result.second[0], egraph.find(a));
  EXPECT_EQ(result.second[1], egraph.find(b));
  EXPECT_THROW((void)extractor.solveWithTimeout(a, 0, -1.0),
               std::runtime_error);
}

LOTUS_EGRAPH_DEFINE_LANGUAGE(TestLang, {
  LOTUS_EGRAPH_LANG_OP("+", 2)
  LOTUS_EGRAPH_LANG_OP("*", 2)
  LOTUS_EGRAPH_LANG_OP("x", 0)
  LOTUS_EGRAPH_LANG_OP("y", 0)
});

TEST(EGraphFeatureTest, DefineLanguageSurfaceConstrainsFromOp) {
  auto plus = RecExpr<TestLang>::parse("(+ x y)");
  EXPECT_EQ(plus.toString(), "(+ x y)");

  EGraph<TestLang> egraph;
  Id root = egraph.addExpr(plus);
  egraph.rebuild();

  auto pat = Pattern<TestLang>::parse("(+ ?a ?b)");
  auto matches = pat.search(egraph);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(egraph.find(matches.front().eclass), egraph.find(root));

  EXPECT_THROW((void)RecExpr<TestLang>::parse("z"), std::runtime_error);
  EXPECT_THROW((void)RecExpr<TestLang>::parse("(- x y)"), std::runtime_error);
}
