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
