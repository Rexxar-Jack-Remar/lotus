#include "EGraphFeatureTestSupport.h"

TEST(EGraphFeatureTest, ExtractorAstSizeOverflowKeepsMonotoneCosting) {
  std::vector<Rewrite<SymbolLang>> rules = {
      makeRewrite<SymbolLang>("explode", "(meow ?a)", "(meow (meow ?a ?a))")};

  auto start = RecExpr<SymbolLang>::parse("(meow 42)");
  Runner<SymbolLang> runner;
  runner.withIterLimit(100).withExpr(start).run(rules);

  Extractor<SymbolLang> extractor(runner.egraph);
  auto best = extractor.findBest(runner.roots[0]).second;
  EXPECT_EQ(best.toString(), start.toString());
}
TEST(EGraphFeatureTest, ExtractorSupportsEggStyleMutableCostFunctionApi) {
  EGraph<SymbolLang> egraph;
  Id root = egraph.addExpr(
      RecExpr<SymbolLang>::parse("(root (cheap a) (expensive b))"));
  egraph.rebuild();

  CountingCostFn cost_fn;
  EXPECT_EQ(cost_fn.costRec(
                RecExpr<SymbolLang>::parse("(root (cheap a) (expensive b))")),
            21u);
  EXPECT_GT(cost_fn.calls, 0u);

  Extractor<SymbolLang, NoAnalysis<SymbolLang>, CountingCostFn> extractor(
      egraph, std::move(cost_fn));
  auto [cost, best] = extractor.findBest(root);
  EXPECT_EQ(cost, 21u);
  EXPECT_EQ(best.toString(), "(root (cheap a) (expensive b))");
  EXPECT_EQ(extractor.findBestCost(root), 21u);
  EXPECT_EQ(extractor.findBestNode(root).op(), "root");
}
TEST(EGraphFeatureTest, AstDepthCostRecMatchesEggStyleDepthMetric) {
  AstDepth<SymbolLang> depth;
  auto expr = RecExpr<SymbolLang>::parse("(f (g (h a)) b)");
  EXPECT_EQ(depth.costRec(expr), 4u);
}
TEST(EGraphFeatureTest, AstDepthSaturatesOnOverflowingChildDepth) {
  AstDepth<SymbolLang> depth;
  SymbolLang node(Symbol("f"), {Id::fromIndex(0)});

  EXPECT_EQ(
      depth.cost(node, [&](Id) { return std::numeric_limits<size_t>::max(); }),
      std::numeric_limits<size_t>::max());
}
TEST(EGraphFeatureTest,
     ExtractorKeepsExtractableLeafRepresentativeInCycleClass) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.add(SymbolLang::leaf("a"));
  Id fa = egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  Id root = egraph.addExpr(RecExpr<SymbolLang>::parse("(g ok)"));
  egraph.rebuild();
  egraph.unite(a, fa);
  egraph.rebuild();

  Extractor<SymbolLang> extractor(egraph);
  auto [cost, best] = extractor.findBest(root);
  EXPECT_EQ(cost, 2u);
  EXPECT_EQ(best.toString(), "(g ok)");
  auto [cyclic_cost, cyclic_best] = extractor.findBest(a);
  EXPECT_EQ(cyclic_cost, 1u);
  EXPECT_EQ(cyclic_best.toString(), "a");
}
TEST(EGraphFeatureTest, AnalysisPendingQueueDeduplicatesParentsLikeEgg) {
  DedupAnalysis::reset();
  EGraph<SymbolLang, DedupAnalysis> egraph;

  Id x = egraph.add(SymbolLang::leaf("x"));
  Id y = egraph.add(SymbolLang::leaf("y"));
  Id plus1 = egraph.add(SymbolLang(Symbol("+"), {x, y}));
  Id plus2 = egraph.add(SymbolLang(Symbol("+"), {x, y}));
  (void)plus1;
  (void)plus2;
  egraph.rebuild();

  DedupAnalysis::reset();
  egraph.setAnalysisData(x, 7);
  egraph.rebuild();

  EXPECT_EQ(DedupAnalysis::remake_calls, 1);
}
TEST(EGraphFeatureTest, LanguageMapperDropsExplanationStateLikeEgg) {
  EGraph<SymbolLang> source = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = source.addUncanonical(SymbolLang::leaf("a"));
  Id b = source.addUncanonical(SymbolLang::leaf("b"));
  source.unite(a, b, "manual");
  Id fa = source.addUncanonical(SymbolLang(Symbol("f"), {a}));
  Id fb = source.addUncanonical(SymbolLang(Symbol("f"), {b}));
  source.rebuild();

  SimpleLanguageMapper<SymbolLang, NoAnalysis<SymbolLang>, SymbolLang,
                       NoAnalysis<SymbolLang>>
      mapper;
  auto mapped = mapper.mapEGraph(source);

  EXPECT_FALSE(mapped.areExplanationsEnabled());
  EXPECT_TRUE(mapped.clean());
  EXPECT_EQ(mapped.addExpr(RecExpr<SymbolLang>::parse("a")),
            mapped.addExpr(RecExpr<SymbolLang>::parse("b")));
  EXPECT_EQ(mapped.idToExpr(fa).toString(), "(f a)");
  EXPECT_EQ(mapped.idToExpr(fb).toString(), "(f a)");
  EXPECT_TRUE(mapped.unionEvents().empty());
  EXPECT_TRUE(mapped.explanationNodes().empty());
  EXPECT_FALSE(explainEquivalence(mapped, a, b).has_value());
}
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
