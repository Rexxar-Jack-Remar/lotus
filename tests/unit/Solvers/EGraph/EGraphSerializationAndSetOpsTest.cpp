#include "EGraphFeatureTestSupport.h"

TEST(EGraphFeatureTest, DotWrapperCanSerializeToFile) {
#if !LOTUS_EGRAPH_ENABLE_DOT
  GTEST_SKIP() << "DOT support disabled at compile time";
#else
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
#endif
}
TEST(EGraphFeatureTest, DotRunMatchesEggBehaviorWhenGraphvizIsAvailable) {
#if !LOTUS_EGRAPH_ENABLE_DOT
  GTEST_SKIP() << "DOT support disabled at compile time";
#else
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.rebuild();

  Dot<SymbolLang, NoAnalysis<SymbolLang>> dot(egraph);
  if (std::system("command -v dot >/dev/null 2>&1") != 0) {
    GTEST_SKIP() << "Graphviz 'dot' not available";
  }

  EXPECT_NO_THROW(dot.runDot(std::vector<std::string>{"-Tsvg"}));
  EXPECT_NO_THROW(
      dot.run(std::string("dot"), std::vector<std::string>{"-Tsvg"}));
#endif
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

  EXPECT_EQ(
      egraph1.addExpr(RecExpr<DynamicLang>::parse("(ln k)")),
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

  EXPECT_EQ(
      egraph2.addExpr(RecExpr<DynamicLang>::parse("(ln k)")),
      egraph2.addExpr(RecExpr<DynamicLang>::parse("(+ (* k pi) (* k pi))")));

  auto egraph3 = egraph1.egraphIntersect(egraph2, NoAnalysis<DynamicLang>{});
  EXPECT_EQ(
      egraph3.addExpr(RecExpr<DynamicLang>::parse("(ln k)")),
      egraph3.addExpr(RecExpr<DynamicLang>::parse("(+ (* k pi) (* k pi))")));
}
TEST(EGraphFeatureTest, EGraphJsonRoundTripsCoreShape) {
#if !LOTUS_EGRAPH_ENABLE_JSON
  GTEST_SKIP() << "JSON support disabled at compile time";
#else
  EGraph<SymbolLang> egraph;
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f a)"));
  egraph.addExpr(RecExpr<SymbolLang>::parse("(f b)"));
  egraph.unite(egraph.addExpr(RecExpr<SymbolLang>::parse("a")),
               egraph.addExpr(RecExpr<SymbolLang>::parse("b")), "eq");
  egraph.rebuild();

  auto json = egraph.toJson();
  auto restored = EGraph<SymbolLang>::fromJson(json);
  restored.rebuild();

  EXPECT_TRUE(restored.clean());
  EXPECT_EQ(restored.totalSize(), 3u);
  EXPECT_EQ(restored.numberOfClasses(), 2u);
  EXPECT_EQ(restored.addExpr(RecExpr<SymbolLang>::parse("a")),
            restored.addExpr(RecExpr<SymbolLang>::parse("b")));
  EXPECT_EQ(restored.addExpr(RecExpr<SymbolLang>::parse("(f a)")),
            restored.addExpr(RecExpr<SymbolLang>::parse("(f b)")));
#endif
}
TEST(EGraphFeatureTest, EGraphJsonParseRejectsMalformedInput) {
#if !LOTUS_EGRAPH_ENABLE_JSON
  GTEST_SKIP() << "JSON support disabled at compile time";
#else
  EXPECT_THROW((void)EGraph<SymbolLang>::parseJson("{not valid json}"),
               std::runtime_error);
  EXPECT_THROW((void)EGraph<SymbolLang>::fromJson(json11::Json::object{}),
               std::runtime_error);
  EXPECT_THROW(
      (void)EGraph<SymbolLang>::parseJson(
          "{\"classes\":["
          "{\"id\":0,\"nodes\":[{\"op\":\"f\",\"children\":[1]}]},"
          "{\"id\":1,\"nodes\":[{\"op\":\"g\",\"children\":[0]}]}]}"),
      std::runtime_error);
  EXPECT_THROW(
      (void)EGraph<SymbolLang>::parseJson(
          R"({"classes":[{"id":0.5,"nodes":[{"op":"a","children":[]}]}]})"),
      std::runtime_error);
#endif
}

TEST(EGraphFeatureTest, EGraphJsonAcceptsFullUnsignedIdRange) {
#if !LOTUS_EGRAPH_ENABLE_JSON
  GTEST_SKIP() << "JSON support disabled at compile time";
#else
  auto restored = EGraph<SymbolLang>::parseJson(
      R"({"classes":[{"id":2147483648,"nodes":[{"op":"wide","children":[]}]}]})");
  restored.rebuild();

  EXPECT_EQ(restored.totalSize(), 1u);
  EXPECT_TRUE(restored.lookup(SymbolLang::leaf("wide")).has_value());
#endif
}
