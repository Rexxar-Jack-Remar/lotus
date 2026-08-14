#include "EGraphFeatureTestSupport.h"

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

#if LOTUS_EGRAPH_ENABLE_DOT
  std::string dot = toDot(egraph);
  EXPECT_NE(dot.find("digraph egraph"), std::string::npos);
#endif
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
TEST(EGraphFeatureTest, IdToExprUsesCanonicalRepresentativeTerm) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  egraph.rebuild();

  egraph.unite(a, b, "manual");
  egraph.rebuild();

  EXPECT_EQ(egraph.idToExpr(a).toString(), "a");
  EXPECT_EQ(egraph.idToExpr(b).toString(), "a");
  EXPECT_EQ(egraph.originalExpr(b).toString(), "b");
}
TEST(EGraphFeatureTest, IdToExprPreservesUncanonicalShapeWithExplanations) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.addUncanonical(SymbolLang::leaf("a"));
  Id b = egraph.addUncanonical(SymbolLang::leaf("b"));
  egraph.unite(a, b, "manual");
  egraph.rebuild();

  Id fa = egraph.addUncanonical(SymbolLang(Symbol("f"), {a}));
  Id fb = egraph.addUncanonical(SymbolLang(Symbol("f"), {b}));
  egraph.rebuild();

  EXPECT_EQ(egraph.idToExpr(fa).toString(), "(f a)");
  EXPECT_EQ(egraph.idToExpr(fb).toString(), "(f b)");
  EXPECT_EQ(egraph.originalExpr(fa).toString(), "(f a)");
  EXPECT_EQ(egraph.originalExpr(fb).toString(), "(f b)");
}
TEST(EGraphFeatureTest, DisablingExplanationsDropsUncanonicalIdentity) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.addUncanonical(SymbolLang::leaf("a"));
  Id b = egraph.addUncanonical(SymbolLang::leaf("b"));
  egraph.unite(a, b, "manual");
  egraph.rebuild();

  Id fa = egraph.addUncanonical(SymbolLang(Symbol("f"), {a}));
  Id fb = egraph.addUncanonical(SymbolLang(Symbol("f"), {b}));
  egraph.rebuild();

  EXPECT_EQ(egraph.idToExpr(fa).toString(), "(f a)");
  EXPECT_EQ(egraph.idToExpr(fb).toString(), "(f b)");

  auto disabled = egraph.withExplanationsDisabled();
  EXPECT_FALSE(disabled.areExplanationsEnabled());
  EXPECT_EQ(disabled.idToExpr(fa).toString(), "(f a)");
  EXPECT_EQ(disabled.idToExpr(fb).toString(), "(f a)");
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
TEST(EGraphFeatureTest, ExplanationPreservesAlternateRewriteWithinSameClass) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  egraph.unionInstantiations(Pattern<SymbolLang>::parse("a").ast(),
                             Pattern<SymbolLang>::parse("b").ast(), Subst{},
                             "a_to_b");
  egraph.rebuild();

  egraph.unionInstantiations(Pattern<SymbolLang>::parse("a").ast(),
                             Pattern<SymbolLang>::parse("b").ast(), Subst{},
                             "a_to_b_again");
  egraph.rebuild();

  auto explanation = explainEquivalence(egraph, a, b);
  ASSERT_TRUE(explanation.has_value());
  const auto &flat = explanation->makeFlatExplanation();
  ASSERT_GE(flat.size(), 2u);
  EXPECT_EQ(flat.back().forward_rule,
            std::optional<Symbol>(Symbol("a_to_b_again")));
}
TEST(EGraphFeatureTest, ExplanationCheckProofAcceptsEggStyleProofs) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  auto rules = makeExplanationRules();
  RecExpr<SymbolLang> fa = RecExpr<SymbolLang>::parse("(f a)");
  RecExpr<SymbolLang> fb = RecExpr<SymbolLang>::parse("(f b)");
  egraph.addExpr(fa);
  egraph.addExpr(fb);
  egraph.addExpr(RecExpr<SymbolLang>::parse("c"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("d"));

  egraph.unionInstantiations(Pattern<SymbolLang>::parse("a").ast(),
                             Pattern<SymbolLang>::parse("c").ast(), Subst{},
                             "ac");
  egraph.unionInstantiations(Pattern<SymbolLang>::parse("c").ast(),
                             Pattern<SymbolLang>::parse("d").ast(), Subst{},
                             "cd");
  egraph.unionInstantiations(Pattern<SymbolLang>::parse("d").ast(),
                             Pattern<SymbolLang>::parse("b").ast(), Subst{},
                             "db");
  egraph.rebuild();

  auto explanation = explainEquivalence(egraph, fa, fb);
  ASSERT_TRUE(explanation.has_value());
  auto check = [&] {
    explanation->template checkProof<const std::vector<Rewrite<SymbolLang>> &,
                                     NoAnalysis<SymbolLang>>(rules);
  };
  EXPECT_NO_THROW(check());
}
TEST(EGraphFeatureTest,
     CheckEachExplainIgnoresAlternateRewriteNeighborsLikeEgg) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  egraph.rebuild();

  egraph.unionInstantiations(Pattern<SymbolLang>::parse("a").ast(),
                             Pattern<SymbolLang>::parse("b").ast(), Subst{},
                             "good");
  egraph.rebuild();

  egraph.unionInstantiations(Pattern<SymbolLang>::parse("a").ast(),
                             Pattern<SymbolLang>::parse("b").ast(), Subst{},
                             "bad");
  egraph.rebuild();

  std::vector<Rewrite<SymbolLang>> rules = {
      makeRewrite<SymbolLang>("good", "a", "b"),
      makeRewrite<SymbolLang>("bad", "(f ?x)", "?x"),
  };

  EXPECT_TRUE(checkEachExplain(egraph, rules));
}
TEST(EGraphFeatureTest,
     ExplanationKeepsDistinctRuleAnnotationsForSharedTargetNodes) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  Id c = egraph.addExpr(RecExpr<SymbolLang>::parse("c"));
  RecExpr<SymbolLang> left = RecExpr<SymbolLang>::parse("(pair a c)");
  RecExpr<SymbolLang> right = RecExpr<SymbolLang>::parse("(pair b b)");
  egraph.addExpr(left);
  egraph.addExpr(right);

  egraph.unionInstantiations(Pattern<SymbolLang>::parse("a").ast(),
                             Pattern<SymbolLang>::parse("b").ast(), Subst{},
                             "ab");
  egraph.unionInstantiations(Pattern<SymbolLang>::parse("c").ast(),
                             Pattern<SymbolLang>::parse("b").ast(), Subst{},
                             "cb");
  egraph.rebuild();

  auto explanation = explainEquivalence(egraph, left, right);
  ASSERT_TRUE(explanation.has_value());
  const auto &flat = explanation->makeFlatExplanation();
  ASSERT_EQ(flat.size(), 3u);
  ASSERT_EQ(flat[1].children.size(), 2u);
  ASSERT_EQ(flat[2].children.size(), 2u);
  EXPECT_EQ(flat[1].children[0].forward_rule,
            std::optional<Symbol>(Symbol("ab")));
  EXPECT_EQ(flat[1].children[1].forward_rule, std::nullopt);
  EXPECT_EQ(flat[2].children[0].forward_rule, std::nullopt);
  EXPECT_EQ(flat[2].children[1].forward_rule,
            std::optional<Symbol>(Symbol("cb")));
}
TEST(EGraphFeatureTest, ExplanationStringWithLetUsesPointerSharingOnly) {
  auto mk_leaf = [](std::string_view expr_text) {
    auto term = std::make_shared<TreeTerm<SymbolLang>>();
    term->expr = RecExpr<SymbolLang>::parse(expr_text);
    return term;
  };

  auto shared_child = std::make_shared<TreeTerm<SymbolLang>>();
  shared_child->expr = RecExpr<SymbolLang>::parse("(g x)");
  shared_child->child_proofs = {{mk_leaf("x")}};

  auto distinct_a = std::make_shared<TreeTerm<SymbolLang>>();
  distinct_a->expr = RecExpr<SymbolLang>::parse("(g x)");
  distinct_a->child_proofs = {{mk_leaf("x")}};

  auto distinct_b = std::make_shared<TreeTerm<SymbolLang>>();
  distinct_b->expr = RecExpr<SymbolLang>::parse("(g x)");
  distinct_b->child_proofs = {{mk_leaf("x")}};

  auto repeated = std::make_shared<TreeTerm<SymbolLang>>();
  repeated->expr = RecExpr<SymbolLang>::parse("(+ (g x) (g x))");
  repeated->forward_rule = Symbol("shared");
  repeated->child_proofs = {{shared_child}, {shared_child}};

  auto structural = std::make_shared<TreeTerm<SymbolLang>>();
  structural->expr = RecExpr<SymbolLang>::parse("(+ (g x) (g x))");
  structural->forward_rule = Symbol("shared");
  structural->child_proofs = {{distinct_a}, {distinct_b}};

  Explanation<SymbolLang> shared_expl({repeated});
  Explanation<SymbolLang> structural_expl({structural});

  EXPECT_NE(shared_expl.getStringWithLet().find("(let (v_0 "),
            std::string::npos);
  EXPECT_EQ(structural_expl.getStringWithLet().find("(let (v_0 "),
            std::string::npos);
}
TEST(EGraphFeatureTest, FlatTermRewriteMatchesEggProofRewritingModel) {
  auto expr = RecExpr<SymbolLang>::parse("(+ a a)");
  auto term = FlatTerm<SymbolLang>::fromExpr(expr, expr.root());

  auto rewritten = term.rewrite(Pattern<SymbolLang>::parse("(+ ?x ?x)").ast(),
                                Pattern<SymbolLang>::parse("(* 2 ?x)").ast());

  EXPECT_EQ(rewritten.getString(), "(* 2 a)");
  EXPECT_EQ(rewritten.getRecExpr().toString(), "(* 2 a)");
}
TEST(EGraphFeatureTest, IdToPatternPreservesSuppliedUncanonicalId) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.add(SymbolLang::leaf("a"));
  Id b = egraph.add(SymbolLang::leaf("b"));
  egraph.unionInstantiations(Pattern<SymbolLang>::parse("a").ast(),
                             Pattern<SymbolLang>::parse("b").ast(), Subst{},
                             "a_to_b");
  egraph.rebuild();

  auto [pat_a, subst_a] = egraph.idToPattern(a, {});
  auto [pat_b, subst_b] = egraph.idToPattern(b, {});

  EXPECT_TRUE(subst_a.bindings().empty());
  EXPECT_TRUE(subst_b.bindings().empty());
  EXPECT_EQ(pat_a.ast().toString(), "a");
  EXPECT_EQ(pat_b.ast().toString(), "b");
}
TEST(EGraphFeatureTest, UnionTrustedRecordsExplanationReason) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  egraph.rebuild();

  EXPECT_TRUE(egraph.unionTrusted(a, b, "trusted"));
  egraph.rebuild();

  auto explanation = explainEquivalence(egraph, a, b);
  ASSERT_TRUE(explanation.has_value());
  ASSERT_GE(explanation->makeFlatExplanation().size(), 2u);
  EXPECT_EQ(explanation->makeFlatExplanation().back().forward_rule,
            std::optional<Symbol>(Symbol("trusted")));
}
TEST(EGraphFeatureTest, CopyWithoutUnionsRequiresExplanations) {
  EGraph<SymbolLang> egraph;
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  egraph.unite(a, b, "manual");
  egraph.rebuild();

  EXPECT_THROW((void)egraph.copyWithoutUnions(NoAnalysis<SymbolLang>{}),
               std::runtime_error);
}
TEST(EGraphFeatureTest, CopyWithoutUnionsWorksWithExplanations) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  Id a = egraph.addExpr(RecExpr<SymbolLang>::parse("a"));
  Id b = egraph.addExpr(RecExpr<SymbolLang>::parse("b"));
  Id fa = egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.unite(a, b, "manual");
  egraph.rebuild();

  auto copy = egraph.copyWithoutUnions(NoAnalysis<SymbolLang>{});
  copy.rebuild();

  auto copy_a = copy.lookupExpr(RecExpr<SymbolLang>::parse("a"));
  auto copy_b = copy.lookupExpr(RecExpr<SymbolLang>::parse("b"));
  auto copy_fa = copy.lookupExpr(RecExpr<SymbolLang>::parse("(f a)"));
  ASSERT_TRUE(copy_a.has_value());
  ASSERT_TRUE(copy_b.has_value());
  ASSERT_TRUE(copy_fa.has_value());
  EXPECT_NE(copy.find(*copy_a), copy.find(*copy_b));
  EXPECT_EQ(copy.idToExpr(*copy_fa).toString(), "(f a)");
  EXPECT_EQ(copy.numberOfClasses(), 3u);
  EXPECT_EQ(egraph.find(a), egraph.find(b));
  EXPECT_EQ(copy.originalExpr(*copy_fa).toString(), "(f a)");
  EXPECT_NE(copy.find(*copy_fa), copy.find(*copy_a));
  EXPECT_NE(copy.find(*copy_fa), copy.find(*copy_b));
}
