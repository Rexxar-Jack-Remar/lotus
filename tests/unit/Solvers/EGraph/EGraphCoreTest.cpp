#include "Solvers/EGraph.h"

#include <gtest/gtest.h>

using namespace lotus::egraph;

TEST(EGraphCoreTest, RebuildMergesCongruentParents) {
  EGraph<SymbolLang> egraph;

  Id x = egraph.add(SymbolLang::leaf("x"));
  Id y = egraph.add(SymbolLang::leaf("y"));
  Id fx = egraph.add(SymbolLang("f", {x}));
  Id fy = egraph.add(SymbolLang("f", {y}));

  EXPECT_NE(egraph.find(fx), egraph.find(fy));

  egraph.unite(x, y, "eq");
  egraph.rebuild();

  EXPECT_EQ(egraph.find(x), egraph.find(y));
  EXPECT_EQ(egraph.find(fx), egraph.find(fy));
}

TEST(EGraphCoreTest, RebuildRepairsEveryNodeInMergedParentClass) {
  EGraph<SymbolLang> egraph;

  Id x = egraph.add(SymbolLang::leaf("x"));
  Id y = egraph.add(SymbolLang::leaf("y"));
  Id z = egraph.add(SymbolLang::leaf("z"));
  Id fx = egraph.add(SymbolLang("f", {x}));
  Id gy = egraph.add(SymbolLang("g", {y}));
  Id gz = egraph.add(SymbolLang("g", {z}));

  egraph.unite(fx, gy);
  egraph.rebuild();
  ASSERT_NE(egraph.find(fx), egraph.find(gz));

  egraph.unite(z, y);
  egraph.rebuild();

  EXPECT_EQ(egraph.find(fx), egraph.find(gz));
  EXPECT_EQ(egraph.totalSize(), egraph.memoSize());
}

TEST(EGraphCoreTest, LookupUsesCanonicalChildren) {
  EGraph<SymbolLang> egraph;

  Id a = egraph.add(SymbolLang::leaf("a"));
  Id b = egraph.add(SymbolLang::leaf("b"));
  egraph.unite(a, b, "eq");
  egraph.rebuild();

  auto found = egraph.lookup(SymbolLang("g", {a}));
  EXPECT_FALSE(found.has_value());

  Id ga = egraph.add(SymbolLang("g", {a}));
  egraph.rebuild();

  auto gb = egraph.lookup(SymbolLang("g", {b}));
  ASSERT_TRUE(gb.has_value());
  EXPECT_EQ(egraph.find(ga), egraph.find(*gb));
}

TEST(EGraphCoreTest, UnionFindMatchesEggRepresentativeBehavior) {
  UnionFind uf;
  for (size_t i = 0; i < 10; ++i) {
    uf.makeSet();
  }

  uf.unite(Id::fromIndex(0), Id::fromIndex(1));
  uf.unite(Id::fromIndex(0), Id::fromIndex(2));
  uf.unite(Id::fromIndex(0), Id::fromIndex(3));

  uf.unite(Id::fromIndex(6), Id::fromIndex(7));
  uf.unite(Id::fromIndex(6), Id::fromIndex(8));
  uf.unite(Id::fromIndex(6), Id::fromIndex(9));

  for (size_t i = 0; i < 10; ++i) {
    (void)uf.findMut(Id::fromIndex(i));
  }

  EXPECT_EQ(uf.find(Id::fromIndex(0)), Id::fromIndex(0));
  EXPECT_EQ(uf.find(Id::fromIndex(1)), Id::fromIndex(0));
  EXPECT_EQ(uf.find(Id::fromIndex(2)), Id::fromIndex(0));
  EXPECT_EQ(uf.find(Id::fromIndex(3)), Id::fromIndex(0));
  EXPECT_EQ(uf.find(Id::fromIndex(4)), Id::fromIndex(4));
  EXPECT_EQ(uf.find(Id::fromIndex(5)), Id::fromIndex(5));
  EXPECT_EQ(uf.find(Id::fromIndex(6)), Id::fromIndex(6));
  EXPECT_EQ(uf.find(Id::fromIndex(7)), Id::fromIndex(6));
  EXPECT_EQ(uf.find(Id::fromIndex(8)), Id::fromIndex(6));
  EXPECT_EQ(uf.find(Id::fromIndex(9)), Id::fromIndex(6));
}

TEST(EGraphCoreTest, SimpleAddFromEggDoesNotBreakRebuild) {
  EGraph<SymbolLang> egraph = EGraph<SymbolLang>().withExplanationsEnabled();

  Id x = egraph.add(SymbolLang::leaf("x"));
  Id x2 = egraph.add(SymbolLang::leaf("x"));
  Id plus = egraph.add(SymbolLang("+", {x, x2}));

  auto [merged, changed] = egraph.unionInstantiations(
      Pattern<SymbolLang>::parse("x").ast(),
      Pattern<SymbolLang>::parse("y").ast(), Subst{}, "union x and y");
  EXPECT_TRUE(changed);
  egraph.rebuild();

  EXPECT_EQ(egraph.find(x), egraph.find(merged));
  EXPECT_TRUE(egraph.lookup(SymbolLang("+", {x, x2})).has_value());
  EXPECT_EQ(egraph.find(plus),
            egraph.find(*egraph.lookup(SymbolLang("+", {x, x2}))));
}
