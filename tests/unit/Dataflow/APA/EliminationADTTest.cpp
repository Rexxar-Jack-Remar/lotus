#include "EliminationTestSupport.h"

TEST(EliminationTest, ADTDelayedReducible) {
  ReducibleReachabilityProblem Problem;
  elimination::IntraEliminationSolver<TestDomain> Solver(
      Problem, elimination::EliminationOptions{
                   elimination::EliminationMethod::ADTDelayed});
  Solver.solve();
  EXPECT_TRUE(Solver.usedADT());

  const auto &Res = Solver.getResults();
  EXPECT_EQ(factAt(Res, 0), (std::set<int>{}));
  EXPECT_EQ(factAt(Res, 1), (std::set<int>{1, 2}));
  EXPECT_EQ(factAt(Res, 2), (std::set<int>{1, 2}));
  EXPECT_EQ(factAt(Res, 3), (std::set<int>{1, 2, 3}));
}
TEST(EliminationTest, ADTSimpleReducible) {
  ReducibleReachabilityProblem Problem;
  elimination::IntraEliminationSolver<TestDomain> Solver(
      Problem, elimination::EliminationOptions{
                   elimination::EliminationMethod::ADTSimple});
  const auto Status = Solver.solve();
  EXPECT_EQ(Status, elimination::SolveStatus::Ok);
  EXPECT_TRUE(Solver.usedADT());

  const auto &Res = Solver.getResults();
  EXPECT_EQ(factAt(Res, 0), (std::set<int>{}));
  EXPECT_EQ(factAt(Res, 1), (std::set<int>{1, 2}));
  EXPECT_EQ(factAt(Res, 2), (std::set<int>{1, 2}));
  EXPECT_EQ(factAt(Res, 3), (std::set<int>{1, 2, 3}));
}
TEST(EliminationTest, EngineParityOnReducibleGraph) {
  ReducibleReachabilityProblem Problem;
  elimination::IntraEliminationSolver<TestDomain> StateSolver(
      Problem, elimination::EliminationOptions{
                   elimination::EliminationMethod::StateElimination});
  elimination::IntraEliminationSolver<TestDomain> SimpleSolver(
      Problem, elimination::EliminationOptions{
                   elimination::EliminationMethod::ADTSimple});
  elimination::IntraEliminationSolver<TestDomain> DelayedSolver(
      Problem, elimination::EliminationOptions{
                   elimination::EliminationMethod::ADTDelayed});

  EXPECT_EQ(StateSolver.solve(), elimination::SolveStatus::Ok);
  EXPECT_EQ(SimpleSolver.solve(), elimination::SolveStatus::Ok);
  EXPECT_EQ(DelayedSolver.solve(), elimination::SolveStatus::Ok);

  const auto &StateRes = StateSolver.getResults();
  const auto &SimpleRes = SimpleSolver.getResults();
  const auto &DelayedRes = DelayedSolver.getResults();
  for (const auto Node : Problem.nodes()) {
    EXPECT_EQ(factAt(StateRes, Node), factAt(SimpleRes, Node));
    EXPECT_EQ(factAt(StateRes, Node), factAt(DelayedRes, Node));
  }
}
TEST(EliminationTest, ADTFallsBackOnIrreducibleGraph) {
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {1, 2}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> StateSolver(
      Problem, elimination::EliminationOptions{
                   elimination::EliminationMethod::StateElimination});
  elimination::IntraEliminationSolver<TestDomain> ADTSolver(
      Problem, elimination::EliminationOptions{
                   elimination::EliminationMethod::ADTDelayed});

  EXPECT_EQ(StateSolver.solve(), elimination::SolveStatus::Ok);
  EXPECT_EQ(ADTSolver.solve(), elimination::SolveStatus::FallbackToState);
  EXPECT_FALSE(ADTSolver.usedADT());
  EXPECT_EQ(ADTSolver.getDiagnostics().fallback_reason,
            elimination::FallbackReason::ADTRejected);

  const auto &StateRes = StateSolver.getResults();
  const auto &ADTRes = ADTSolver.getResults();
  for (const auto &Node : Problem.nodes()) {
    EXPECT_EQ(factAt(StateRes, Node), factAt(ADTRes, Node));
  }
}
TEST(EliminationTest, NonConvergentStarFailPolicy) {
  NonConvergentProblem Problem;
  elimination::EliminationOptions Opts;
  Opts.MaxStarIterations = 3;
  Opts.NonConvergentStarPolicy = elimination::OnNonConvergentStar::Fail;
  elimination::IntraEliminationSolver<NonConvergentDomain> Solver(Problem,
                                                                  Opts);
  EXPECT_EQ(Solver.solve(), elimination::SolveStatus::NonConvergentStar);
  EXPECT_TRUE(Solver.getDiagnostics().max_star_hit);
}
TEST(EliminationTest, NonConvergentStarReturnLastPolicy) {
  NonConvergentProblem Problem;
  elimination::EliminationOptions Opts;
  Opts.MaxStarIterations = 3;
  Opts.NonConvergentStarPolicy = elimination::OnNonConvergentStar::ReturnLast;
  elimination::IntraEliminationSolver<NonConvergentDomain> Solver(Problem,
                                                                  Opts);
  EXPECT_EQ(Solver.solve(), elimination::SolveStatus::Ok);
  const auto &Res = Solver.getResults();
  ASSERT_NE(Res.tryIN(0), nullptr);
  EXPECT_EQ(*Res.tryIN(0), 1);
}
TEST(EliminationTest, NonConvergentStarReturnIdentityPolicy) {
  NonConvergentProblem Problem;
  elimination::EliminationOptions Opts;
  Opts.MaxStarIterations = 3;
  Opts.NonConvergentStarPolicy =
      elimination::OnNonConvergentStar::ReturnIdentity;
  elimination::IntraEliminationSolver<NonConvergentDomain> Solver(Problem,
                                                                  Opts);
  EXPECT_EQ(Solver.solve(), elimination::SolveStatus::Ok);
  EXPECT_TRUE(Solver.getDiagnostics().max_star_hit);
  const auto &Res = Solver.getResults();
  ASSERT_NE(Res.tryIN(0), nullptr);
  EXPECT_NE(*Res.tryIN(0), 1);
}
