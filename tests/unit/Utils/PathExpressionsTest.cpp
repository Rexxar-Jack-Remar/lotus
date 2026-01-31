// Path expressions unit test (Tarjan path expression algorithm).

#include "Utils/General/PathExpressions/PathExpressions.h"

#include <gtest/gtest.h>

using namespace lotus::pathexpressions;

TEST(PathExpressionsTest, TwoNodesOneEdge) {
  GenericLabeledGraph<int, char> g;
  g.addNode(0);
  g.addNode(1);
  EXPECT_TRUE(g.addEdge(0, 'a', 1));
  EXPECT_FALSE(g.addEdge(0, 'a', 1)); // set semantics: duplicate edge ignored

  PathExpressionComputer<int, char> comp(g);
  auto expr_0_0 = comp.exprBetween(0, 0);
  auto expr_0_1 = comp.exprBetween(0, 1);
  auto expr_1_0 = comp.exprBetween(1, 0);

  EXPECT_TRUE(expr_0_0->isEpsilon());   // empty path
  EXPECT_FALSE(expr_0_1->isEpsilon());
  EXPECT_FALSE(expr_0_1->isEmptySet());  // path 0 -a-> 1
  EXPECT_TRUE(expr_1_0->isEmptySet());  // no path 1 -> 0
}

TEST(PathExpressionsTest, ThreeNodesTwoPaths) {
  GenericLabeledGraph<int, char> g;
  g.addNode(0);
  g.addNode(1);
  g.addNode(2);
  g.addEdge(0, 'a', 1);
  g.addEdge(1, 'b', 2);
  g.addEdge(0, 'c', 2);

  PathExpressionComputer<int, char> comp(g);
  auto expr_0_2 = comp.exprBetween(0, 2);

  EXPECT_FALSE(expr_0_2->isEmptySet());
  EXPECT_FALSE(expr_0_2->isEpsilon());
  EXPECT_TRUE(comp.exprBetween(0, 0)->isEpsilon());
  EXPECT_FALSE(comp.exprBetween(0, 1)->isEmptySet());
  EXPECT_FALSE(comp.exprBetween(0, 2)->isEmptySet());
}

TEST(PathExpressionsTest, SelfLoop) {
  GenericLabeledGraph<int, char> g;
  g.addNode(0);
  g.addEdge(0, 'x', 0);

  PathExpressionComputer<int, char> comp(g);
  auto expr_0_0 = comp.exprBetween(0, 0);
  // Paths: ε, x, xx, xxx, ...  =>  x*
  EXPECT_FALSE(expr_0_0->isEmptySet());
  EXPECT_FALSE(expr_0_0->isEpsilon());  // we get star(literal('x')) which is not epsilon
}

TEST(PathExpressionsTest, RegexToTgf) {
  auto a = Regex<char>::literal('a');
  auto b = Regex<char>::literal('b');
  auto u = Regex<char>::union_(a, b);
  const std::string tgf = RegexToTgf<char>::apply(u);
  EXPECT_EQ(tgf, std::string("0 ∪\n1 a\n2 b\n#\n0 1 0\n0 2 1\n"));
}

TEST(PathExpressionsTest, RegexToCompactTgf) {
  auto a = Regex<char>::literal('a');
  auto b = Regex<char>::literal('b');
  auto c = Regex<char>::literal('c');
  auto u1 = Regex<char>::union_(a, b);
  auto u2 = Regex<char>::union_(u1, c); // ((a ∪ b) ∪ c)
  const std::string tgf = RegexToCompactTgf<char>::apply(u2);
  EXPECT_EQ(tgf, std::string("0 ∪\n1 a\n2 b\n3 c\n#\n0 1 0\n0 2 1\n0 3 2\n"));
}
