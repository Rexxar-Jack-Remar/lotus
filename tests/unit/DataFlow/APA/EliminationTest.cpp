/**
 * @file EliminationTest.cpp
 * @brief Unit tests for elimination-based (state elimination) dataflow solver
 */

#include "Dataflow/APA/DataFlow.h"

#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

template <typename ResultT, typename NodeT>
const typename ResultT::fact_t &factAt(const ResultT &Res, const NodeT &N) {
  auto *Fact = Res.tryIN(N);
  EXPECT_NE(Fact, nullptr);
  static const typename ResultT::fact_t Empty{};
  return Fact != nullptr ? *Fact : Empty;
}

struct TestDomain {
  using n_t = int;
  using fact_t = std::set<int>;
  using transfer_t = int; // "gen label"
};

class ReachabilityProblem final
    : public elimination::IntraEliminationProblem<TestDomain> {
public:
  explicit ReachabilityProblem(int Entry,
                               std::unordered_map<int, std::vector<int>> Succs)
      : Entry(Entry), Succs(std::move(Succs)) {}

  std::vector<int> nodes() const override {
    std::vector<int> Ns;
    Ns.reserve(Succs.size());
    for (const auto &It : Succs) {
      Ns.push_back(It.first);
    }
    return Ns;
  }

  int entry() const override { return Entry; }

  std::vector<int> succs(int Node) const override {
    auto It = Succs.find(Node);
    if (It == Succs.end()) {
      return {};
    }
    return It->second;
  }

  int edgeTransfer(int /*Src*/, int Dst) const override { return Dst; }

  fact_t applyTransfer(const int &T, const fact_t &In) const override {
    fact_t Out = In;
    Out.insert(T);
    return Out;
  }

  fact_t meet(const fact_t &Lhs, const fact_t &Rhs) const override {
    fact_t Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t meetIdentity() const override { return {}; }

  fact_t initialFact() const override { return {}; }

  std::size_t maxStarIterations() const override { return 1000; }

private:
  int Entry;
  std::unordered_map<int, std::vector<int>> Succs;
};

} // namespace

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

namespace {

class ReducibleReachabilityProblem final
    : public elimination::IntraReducibleEliminationProblem<TestDomain> {
public:
  std::vector<int> nodes() const override { return {0, 1, 2, 3}; }

  int entry() const override { return 0; }

  std::vector<int> succs(int Node) const override {
    switch (Node) {
    case 0:
      return {1};
    case 1:
      return {2, 3};
    case 2:
      return {1};
    case 3:
    default:
      return {};
    }
  }

  std::vector<Edge> edges() const override {
    return {{0, 1}, {1, 2}, {2, 1}, {1, 3}};
  }

  std::vector<int> topologicalOrder() const override { return {0, 1, 2, 3}; }

  int idom(int Node) const override {
    switch (Node) {
    case 0:
    case 1:
      return 0;
    case 2:
    case 3:
      return 1;
    default:
      return 0;
    }
  }

  bool dominates(int A, int B) const override {
    if (A == B) {
      return true;
    }
    if (A == 0) {
      return true;
    }
    if (A == 1) {
      return B == 2 || B == 3;
    }
    return false;
  }

  int edgeTransfer(int /*Src*/, int Dst) const override { return Dst; }

  fact_t applyTransfer(const int &T, const fact_t &In) const override {
    fact_t Out = In;
    Out.insert(T);
    return Out;
  }

  fact_t meet(const fact_t &Lhs, const fact_t &Rhs) const override {
    fact_t Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t meetIdentity() const override { return {}; }

  fact_t initialFact() const override { return {}; }

  std::size_t maxStarIterations() const override { return 1000; }
};

} // namespace

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

namespace {

struct NonConvergentDomain {
  using n_t = int;
  using fact_t = int;
  using transfer_t = int;
};

class NonConvergentProblem final
    : public elimination::IntraEliminationProblem<NonConvergentDomain> {
public:
  std::vector<int> nodes() const override { return {0}; }
  int entry() const override { return 0; }
  std::vector<int> succs(int) const override { return {0}; }
  transfer_t edgeTransfer(int, int) const override { return 0; }
  fact_t applyTransfer(const transfer_t &, const fact_t &In) const override {
    return 1 - In;
  }
  fact_t meet(const fact_t &, const fact_t &Rhs) const override { return Rhs; }
  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }
  fact_t meetIdentity() const override { return -1; }
  fact_t initialFact() const override { return 0; }
  std::size_t maxStarIterations() const override { return 100; }
};

} // namespace

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

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
