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

TEST(EGraphFeatureTest, PatternSearchWithLimitMatchesEggCounts) {
  auto init_expr =
      RecExpr<SymbolLang>::parse("(+ 1 (+ 2 (+ 3 (+ 4 (+ 5 6)))))");
  std::vector<Rewrite<SymbolLang>> rules = {
      makeRewrite<SymbolLang>("comm", "(+ ?x ?y)", "(+ ?y ?x)"),
      makeRewrite<SymbolLang>("assoc", "(+ ?x (+ ?y ?z))",
                              "(+ (+ ?x ?y) ?z)"),
  };

  Runner<SymbolLang> runner;
  runner.withExpr(init_expr).run(rules);
  const auto &egraph = runner.egraph;

  auto len = [](const std::vector<SearchMatches<SymbolLang>> &matches) {
    size_t total = 0;
    for (const auto &match : matches) {
      total += match.substs.size();
    }
    return total;
  };

  auto pat = Pattern<SymbolLang>::parse("(+ ?x (+ ?y ?z))");
  auto matches = pat.search(egraph);
  EXPECT_EQ(len(matches), 2100u);

  for (size_t limit : {1u, 10u, 100u, 1000u, 10000u}) {
    auto limited = pat.searchWithLimit(egraph, limit);
    EXPECT_EQ(len(limited), std::min(limit, size_t(2100)));
  }

  auto root = egraph.lookupExpr(init_expr);
  ASSERT_TRUE(root.has_value());
  auto eclass_matches = pat.searchEClass(egraph, *root);
  ASSERT_TRUE(eclass_matches.has_value());
  EXPECT_EQ(eclass_matches->substs.size(), 540u);

  for (size_t limit : {1u, 10u, 100u, 1000u}) {
    auto limited = pat.searchEClassWithLimit(egraph, *root, limit);
    ASSERT_TRUE(limited.has_value());
    EXPECT_EQ(limited->substs.size(), std::min(limit, size_t(540)));
  }
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

TEST(EGraphFeatureTest, MultiRewriteRejectsUnboundVariableInApplier) {
  EXPECT_THROW(
      (void)makeMultiRewrite<SymbolLang>(
          "foo", MultiPattern<SymbolLang>::parse("?x = (foo ?y)"),
          MultiPattern<SymbolLang>::parse("?x = ?z")),
      std::runtime_error);
}

TEST(EGraphFeatureTest, MultiRewriteAllowsLaterBindingsIntroducedInApplier) {
  EXPECT_NO_THROW((void)makeMultiRewrite<SymbolLang>(
      "foo", MultiPattern<SymbolLang>::parse("?x = (foo ?y)"),
      MultiPattern<SymbolLang>::parse("?z = (baz ?y), ?x = ?z")));
}

TEST(EGraphFeatureTest, MultiPatternSupportsUnboundRhsIntroductions) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("x"));

  std::vector<Rewrite<SymbolLang>> rules = {
      makeMultiRewrite<SymbolLang>(
          "rule1", MultiPattern<SymbolLang>::parse("?x = x"),
          MultiPattern<SymbolLang>::parse("?y = y, ?y = z")),
      makeMultiRewrite<SymbolLang>(
          "rule2", MultiPattern<SymbolLang>::parse("?x = x, ?y = y, ?z = z"),
          MultiPattern<SymbolLang>::parse("?y = y, ?y = z")),
  };

  Runner<SymbolLang> runner;
  runner.withEGraph(std::move(egraph)).run(rules);
  Id y = runner.egraph.addExpr(RecExpr<SymbolLang>::parse("y"));
  Id z = runner.egraph.addExpr(RecExpr<SymbolLang>::parse("z"));
  EXPECT_EQ(runner.egraph.find(y), runner.egraph.find(z));
}

TEST(EGraphFeatureTest, MultiPatternContextTransferMatchesEgg) {
  EGraph<SymbolLang> egraph;
  auto add_string = [&](std::string_view text) {
    return egraph.addExpr(RecExpr<SymbolLang>::parse(text));
  };

  add_string("(lte ctx1 ctx2)");
  add_string("(lte ctx2 ctx2)");
  add_string("(lte ctx1 ctx1)");
  Id x2 = add_string("(tag x ctx2)");
  Id y2 = add_string("(tag y ctx2)");
  Id z2 = add_string("(tag z ctx2)");

  Id x1 = add_string("(tag x ctx1)");
  Id y1 = add_string("(tag y ctx1)");
  Id z1 = add_string("(tag z ctx2)");
  egraph.unite(x1, y1);
  egraph.unite(y2, z2);

  std::vector<Rewrite<SymbolLang>> rules = {makeMultiRewrite<SymbolLang>(
      "context-transfer",
      MultiPattern<SymbolLang>::parse(
          "?x = (tag ?a ?ctx1) = (tag ?b ?ctx1), "
          "?t = (lte ?ctx1 ?ctx2), "
          "?a1 = (tag ?a ?ctx2), "
          "?b1 = (tag ?b ?ctx2)"),
      MultiPattern<SymbolLang>::parse("?a1 = ?b1"))};

  Runner<SymbolLang> runner;
  runner.withEGraph(std::move(egraph)).run(rules);

  EXPECT_EQ(runner.egraph.find(x1), runner.egraph.find(y1));
  EXPECT_EQ(runner.egraph.find(y2), runner.egraph.find(z2));
  EXPECT_EQ(runner.egraph.find(x2), runner.egraph.find(y2));
  EXPECT_EQ(runner.egraph.find(x2), runner.egraph.find(z2));
  EXPECT_NE(runner.egraph.find(y1), runner.egraph.find(z1));
  EXPECT_NE(runner.egraph.find(x1), runner.egraph.find(z1));
}

TEST(EGraphFeatureTest, MultiPatternAllowsBareVariableAfterFirstClause) {
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.rebuild();

  auto searcher = MultiPattern<SymbolLang>::parse("?a = (f ?x), ?y = ?y");
  auto applier = MultiPattern<SymbolLang>::parse("?a = (g ?x ?y)");
  auto rewrite = makeMultiRewrite<SymbolLang>("r", searcher, applier);

  auto matches = rewrite.search(egraph);
  EXPECT_FALSE(matches.empty());
  EXPECT_NO_THROW((void)rewrite.apply(egraph, matches));
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

namespace {

std::vector<Rewrite<SymbolLang>> makeExplanationRules() {
  return {
      makeRewrite<SymbolLang>("commute-add", "(+ ?a ?b)", "(+ ?b ?a)"),
      makeRewrite<SymbolLang>("commute-mul", "(* ?a ?b)", "(* ?b ?a)"),
      makeRewrite<SymbolLang>("assoc-add", "(+ ?a (+ ?b ?c))", "(+ (+ ?a ?b) ?c)"),
      makeRewrite<SymbolLang>("add-0", "(+ ?a 0)", "?a"),
      makeRewrite<SymbolLang>("mul-1", "(* ?a 1)", "?a"),
      makeRewrite<SymbolLang>("factor", "(+ ?a ?a)", "(* 2 ?a)"),
      makeRewrite<SymbolLang>("cancel-sub", "(- ?a ?a)", "0"),
  };
}

} // namespace

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

TEST(EGraphFeatureTest, ExtractorAstSizeOverflowKeepsMonotoneCosting) {
  std::vector<Rewrite<SymbolLang>> rules = {makeRewrite<SymbolLang>(
      "explode", "(meow ?a)", "(meow (meow ?a ?a))")};

  auto start = RecExpr<SymbolLang>::parse("(meow 42)");
  Runner<SymbolLang> runner;
  runner.withIterLimit(100).withExpr(start).run(rules);

  Extractor<SymbolLang> extractor(runner.egraph);
  auto best = extractor.findBest(runner.roots[0]).second;
  EXPECT_EQ(best.toString(), start.toString());
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

TEST(EGraphFeatureTest, SearchMatchesCarryPerMatchAstIntoAppliers) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f b)"));
  egraph.rebuild();

  auto rewrite = makeRewrite<SymbolLang>("strip-f", "(f ?x)", "?x");
  auto matches = rewrite.search(egraph);
  ASSERT_EQ(matches.size(), 2u);
  for (const auto &match : matches) {
    ASSERT_TRUE(match.ast.has_value());
    EXPECT_EQ(match.ast->toString(), "(f ?x)");
  }

  auto applied = rewrite.apply(egraph, matches);
  EXPECT_EQ(applied.size(), 2u);
  egraph.rebuild();

  auto proof_a = explainEquivalence(egraph, RecExpr<SymbolLang>::parse("(f a)"),
                                    RecExpr<SymbolLang>::parse("a"));
  auto proof_b = explainEquivalence(egraph, RecExpr<SymbolLang>::parse("(f b)"),
                                    RecExpr<SymbolLang>::parse("b"));
  ASSERT_TRUE(proof_a.has_value());
  ASSERT_TRUE(proof_b.has_value());
  EXPECT_EQ(proof_a->makeFlatExplanation().back().forward_rule,
            std::optional<Symbol>(Symbol("strip-f")));
  EXPECT_EQ(proof_b->makeFlatExplanation().back().forward_rule,
            std::optional<Symbol>(Symbol("strip-f")));
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

TEST(EGraphFeatureTest, EGraphUnionAndIntersectMatchEggBasics) {
  auto egraph1 = EGraph<SymbolLang>().withExplanationsEnabled();
  auto egraph2 = EGraph<SymbolLang>().withExplanationsEnabled();

  auto g1 = egraph1;
  auto g2 = egraph2;
  g1.unionInstantiations(Pattern<SymbolLang>::parse("x").ast(),
                         Pattern<SymbolLang>::parse("y").ast(), Subst{}, "");
  g1.unionInstantiations(Pattern<SymbolLang>::parse("y").ast(),
                         Pattern<SymbolLang>::parse("z").ast(), Subst{}, "");
  g2.unionInstantiations(Pattern<SymbolLang>::parse("x").ast(),
                         Pattern<SymbolLang>::parse("y").ast(), Subst{}, "");
  g2.unionInstantiations(Pattern<SymbolLang>::parse("x").ast(),
                         Pattern<SymbolLang>::parse("a").ast(), Subst{}, "");

  auto g3 = g1.egraphIntersect(g2, NoAnalysis<SymbolLang>{});
  g2.egraphUnion(g1);

  EXPECT_EQ(g2.addExpr(RecExpr<SymbolLang>::parse("x")),
            g2.addExpr(RecExpr<SymbolLang>::parse("y")));
  EXPECT_EQ(g3.addExpr(RecExpr<SymbolLang>::parse("x")),
            g3.addExpr(RecExpr<SymbolLang>::parse("y")));

  EXPECT_EQ(g2.addExpr(RecExpr<SymbolLang>::parse("x")),
            g2.addExpr(RecExpr<SymbolLang>::parse("z")));
  EXPECT_NE(g3.addExpr(RecExpr<SymbolLang>::parse("x")),
            g3.addExpr(RecExpr<SymbolLang>::parse("z")));

  EXPECT_EQ(g2.addExpr(RecExpr<SymbolLang>::parse("x")),
            g2.addExpr(RecExpr<SymbolLang>::parse("a")));
  EXPECT_NE(g3.addExpr(RecExpr<SymbolLang>::parse("x")),
            g3.addExpr(RecExpr<SymbolLang>::parse("a")));
}

TEST(EGraphFeatureTest, EGraphIntersectPreservesOnlySharedEqualities) {
  auto g1 = EGraph<SymbolLang>().withExplanationsEnabled();
  auto g2 = EGraph<SymbolLang>().withExplanationsEnabled();

  auto egraph1 = g1;
  auto egraph2 = g2;
  egraph1.unionInstantiations(Pattern<SymbolLang>::parse("(+ x 0)").ast(),
                              Pattern<SymbolLang>::parse("(+ y 0)").ast(),
                              Subst{}, "");
  egraph2.unionInstantiations(Pattern<SymbolLang>::parse("x").ast(),
                              Pattern<SymbolLang>::parse("y").ast(), Subst{},
                              "");
  egraph2.addExpr(RecExpr<SymbolLang>::parse("(+ x 0)"));
  egraph2.addExpr(RecExpr<SymbolLang>::parse("(+ y 0)"));

  auto egraph3 = egraph1.egraphIntersect(egraph2, NoAnalysis<SymbolLang>{});

  EXPECT_NE(egraph3.addExpr(RecExpr<SymbolLang>::parse("x")),
            egraph3.addExpr(RecExpr<SymbolLang>::parse("y")));
  EXPECT_EQ(egraph3.addExpr(RecExpr<SymbolLang>::parse("(+ x 0)")),
            egraph3.addExpr(RecExpr<SymbolLang>::parse("(+ y 0)")));
}

TEST(EGraphFeatureTest, EGraphMediumIntersectMatchesEggExample) {
  EGraph<DynamicLang> egraph1;
  egraph1.addExpr(RecExpr<DynamicLang>::parse("(sqrt (ln 1))"));
  Id ln1 = egraph1.addExpr(RecExpr<DynamicLang>::parse("(ln 1)"));
  Id a = egraph1.addExpr(RecExpr<DynamicLang>::parse("(sqrt (sin pi))"));
  Id b = egraph1.addExpr(RecExpr<DynamicLang>::parse("(* 1 pi)"));
  Id pi = egraph1.addExpr(RecExpr<DynamicLang>::parse("pi"));
  egraph1.unite(a, b);
  egraph1.unite(a, pi);
  Id c = egraph1.addExpr(RecExpr<DynamicLang>::parse("(+ pi pi)"));
  egraph1.unite(ln1, c);
  Id k = egraph1.addExpr(RecExpr<DynamicLang>::parse("k"));
  Id one = egraph1.addExpr(RecExpr<DynamicLang>::parse("1"));
  egraph1.unite(k, one);
  egraph1.rebuild();

  EXPECT_EQ(egraph1.addExpr(RecExpr<DynamicLang>::parse("(ln k)")),
            egraph1.addExpr(RecExpr<DynamicLang>::parse("(+ (* k pi) (* k pi))")));

  EGraph<DynamicLang> egraph2;
  Id ln2 = egraph2.addExpr(RecExpr<DynamicLang>::parse("(ln 2)"));
  Id k2 = egraph2.addExpr(RecExpr<DynamicLang>::parse("k"));
  Id mk1 = egraph2.addExpr(RecExpr<DynamicLang>::parse("(* k 1)"));
  egraph2.unite(mk1, k2);
  Id two = egraph2.addExpr(RecExpr<DynamicLang>::parse("2"));
  egraph2.unite(mk1, two);
  Id mul2pi =
      egraph2.addExpr(RecExpr<DynamicLang>::parse("(+ (* 2 pi) (* 2 pi))"));
  egraph2.unite(ln2, mul2pi);
  egraph2.rebuild();

  EXPECT_EQ(egraph2.addExpr(RecExpr<DynamicLang>::parse("(ln k)")),
            egraph2.addExpr(RecExpr<DynamicLang>::parse("(+ (* k pi) (* k pi))")));

  auto egraph3 = egraph1.egraphIntersect(egraph2, NoAnalysis<DynamicLang>{});
  EXPECT_EQ(egraph3.addExpr(RecExpr<DynamicLang>::parse("(ln k)")),
            egraph3.addExpr(RecExpr<DynamicLang>::parse("(+ (* k pi) (* k pi))")));
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
