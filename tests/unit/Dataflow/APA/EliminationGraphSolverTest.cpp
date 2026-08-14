#include "EliminationTestSupport.h"

TEST(EliminationTest, LoopReachability) {
  // 0 -> 1 -> 2 -> 3
  //      ^    |
  //      |____|
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1}}, {1, {2}}, {2, {1, 3}}, {3, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(factAt(Res, 0), (std::set<int>{}));
  EXPECT_EQ(factAt(Res, 1), (std::set<int>{1, 2}));
  EXPECT_EQ(factAt(Res, 2), (std::set<int>{1, 2}));
  EXPECT_EQ(factAt(Res, 3), (std::set<int>{1, 2, 3}));
}
TEST(EliminationTest, BranchJoinUnion) {
  // 0 -> 1 -> 3
  //  \-> 2 -/
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(factAt(Res, 3), (std::set<int>{1, 2, 3}));
}
TEST(EliminationTest, EmptyGraph) {
  std::unordered_map<int, std::vector<int>> Succs = {};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_FALSE(Res.containsNode(0));
  EXPECT_EQ(Res.tryIN(0), nullptr);
}
TEST(EliminationTest, SingleNodeNoEdges) {
  std::unordered_map<int, std::vector<int>> Succs = {{0, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(factAt(Res, 0), (std::set<int>{}));
}
TEST(EliminationTest, SingleNodeSelfLoop) {
  std::unordered_map<int, std::vector<int>> Succs = {{0, {0}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  // Path from 0 to 0: empty path or one or more self-loops; each adds 0.
  EXPECT_EQ(factAt(Res, 0), (std::set<int>{0}));
}
TEST(EliminationTest, UnreachableNodeGetsMeetIdentity) {
  // 0 -> 0 (self-loop); 1 isolated. Entry 0.
  std::unordered_map<int, std::vector<int>> Succs = {{0, {0}}, {1, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(factAt(Res, 0), (std::set<int>{0}));
  EXPECT_EQ(factAt(Res, 1), (std::set<int>{})); // unreachable => meet identity
}
TEST(EliminationTest, DisconnectedTwoComponents) {
  // Component 1: 0 -> 0.  Component 2: 1 -> 1.  Entry 0.
  std::unordered_map<int, std::vector<int>> Succs = {{0, {0}}, {1, {1}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(factAt(Res, 0), (std::set<int>{0}));
  EXPECT_EQ(factAt(Res, 1), (std::set<int>{}));
}
TEST(EliminationTest, DiamondWithUnreachableSink) {
  // 0 -> 1 -> 3, 0 -> 2 -> 3; node 4 has no predecessors.
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}, {4, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(factAt(Res, 3), (std::set<int>{1, 2, 3}));
  EXPECT_EQ(factAt(Res, 4), (std::set<int>{})); // unreachable
}
TEST(EliminationTest, ExprToStoredForEveryNode) {
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1}}, {1, {2}}, {2, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  // Every node in the problem gets a path expression (possibly Zero if
  // unreachable).
  EXPECT_TRUE(Res.ExprTo(0));
  EXPECT_TRUE(Res.ExprTo(1));
  EXPECT_TRUE(Res.ExprTo(2));
}
TEST(EliminationTest, PathExprFactoryHashConsesAtomsAndComposites) {
  elimination::PathExprFactory<int> Exprs;

  auto A1 = Exprs.atom(7);
  auto A2 = Exprs.atom(7);
  auto B = Exprs.atom(9);

  EXPECT_EQ(A1, A2);
  EXPECT_NE(A1, B);

  auto U1 = Exprs.unite(A1, B);
  auto U2 = Exprs.unite(A2, B);
  auto U3 = Exprs.unite(B, A1);
  EXPECT_EQ(U1, U2);
  EXPECT_NE(U1, U3);

  auto C1 = Exprs.concat(A1, B);
  auto C2 = Exprs.concat(A2, B);
  auto C3 = Exprs.concat(B, A1);
  EXPECT_EQ(C1, C2);
  EXPECT_NE(C1, C3);

  auto S1 = Exprs.star(U1);
  auto S2 = Exprs.star(U2);
  EXPECT_EQ(S1, S2);
}
TEST(EliminationTest, SolverReusesExpressionNodesAcrossEquivalentPaths) {
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  ASSERT_EQ(Solver.solve(), elimination::SolveStatus::Ok);
  const auto &Res = Solver.getResults();

  auto ExprTo1 = Res.ExprTo(1);
  auto ExprTo2 = Res.ExprTo(2);
  auto ExprTo3 = Res.ExprTo(3);

  ASSERT_TRUE(ExprTo1);
  ASSERT_TRUE(ExprTo2);
  ASSERT_TRUE(ExprTo3);
  ASSERT_EQ(ExprTo1->K, elimination::PathExprFactory<int>::Kind::Atom);
  ASSERT_EQ(ExprTo2->K, elimination::PathExprFactory<int>::Kind::Atom);
  EXPECT_EQ(ExprTo1->Transfer, ExprTo3->L->L->Transfer);
  EXPECT_EQ(ExprTo2->Transfer, ExprTo3->R->L->Transfer);
  EXPECT_EQ(ExprTo3->L->R, ExprTo3->R->R);
}
TEST(EliminationTest, LinearChain) {
  // 0 -> 1 -> 2 -> 3
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1}}, {1, {2}}, {2, {3}}, {3, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(factAt(Res, 0), (std::set<int>{}));
  EXPECT_EQ(factAt(Res, 1), (std::set<int>{1}));
  EXPECT_EQ(factAt(Res, 2), (std::set<int>{1, 2}));
  EXPECT_EQ(factAt(Res, 3), (std::set<int>{1, 2, 3}));
}
