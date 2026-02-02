/**
 * @file EliminationTest.cpp
 * @brief Unit tests for elimination-based (state elimination) dataflow solver
 */

#include "Dataflow/Elimination/DataFlow.h"

#include <gtest/gtest.h>

#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct TestDomain {
  using n_t = int;
  using fact_t = std::set<int>;
  using transfer_t = int; // "gen label"
};

class ReachabilityProblem final
    : public elimination::IntraEliminationProblem<TestDomain> {
public:
  explicit ReachabilityProblem(
      int Entry, std::unordered_map<int, std::vector<int>> Succs)
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

  EXPECT_EQ(Res.IN(0), (std::set<int>{}));
  EXPECT_EQ(Res.IN(1), (std::set<int>{1, 2}));
  EXPECT_EQ(Res.IN(2), (std::set<int>{1, 2}));
  EXPECT_EQ(Res.IN(3), (std::set<int>{1, 2, 3}));
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

  EXPECT_EQ(Res.IN(3), (std::set<int>{1, 2, 3}));
}

TEST(EliminationTest, EmptyGraph) {
  std::unordered_map<int, std::vector<int>> Succs = {};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  // No nodes => no entries in result; IN(any) returns default (meet identity).
  EXPECT_TRUE(Res.IN(0).empty());
}

TEST(EliminationTest, SingleNodeNoEdges) {
  std::unordered_map<int, std::vector<int>> Succs = {{0, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(Res.IN(0), (std::set<int>{}));
}

TEST(EliminationTest, SingleNodeSelfLoop) {
  std::unordered_map<int, std::vector<int>> Succs = {{0, {0}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  // Path from 0 to 0: empty path or one or more self-loops; each adds 0.
  EXPECT_EQ(Res.IN(0), (std::set<int>{0}));
}

TEST(EliminationTest, UnreachableNodeGetsMeetIdentity) {
  // 0 -> 0 (self-loop); 1 isolated. Entry 0.
  std::unordered_map<int, std::vector<int>> Succs = {{0, {0}}, {1, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(Res.IN(0), (std::set<int>{0}));
  EXPECT_EQ(Res.IN(1), (std::set<int>{})); // unreachable => meet identity
}

TEST(EliminationTest, DisconnectedTwoComponents) {
  // Component 1: 0 -> 0.  Component 2: 1 -> 1.  Entry 0.
  std::unordered_map<int, std::vector<int>> Succs = {{0, {0}}, {1, {1}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(Res.IN(0), (std::set<int>{0}));
  EXPECT_EQ(Res.IN(1), (std::set<int>{}));
}

TEST(EliminationTest, DiamondWithUnreachableSink) {
  // 0 -> 1 -> 3, 0 -> 2 -> 3; node 4 has no predecessors.
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}, {4, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  EXPECT_EQ(Res.IN(3), (std::set<int>{1, 2, 3}));
  EXPECT_EQ(Res.IN(4), (std::set<int>{})); // unreachable
}

TEST(EliminationTest, ExprToStoredForEveryNode) {
  std::unordered_map<int, std::vector<int>> Succs = {
      {0, {1}}, {1, {2}}, {2, {}}};
  ReachabilityProblem Problem(0, Succs);

  elimination::IntraEliminationSolver<TestDomain> Solver(Problem);
  Solver.solve();
  const auto &Res = Solver.getResults();

  // Every node in the problem gets a path expression (possibly Zero if unreachable).
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

  EXPECT_EQ(Res.IN(0), (std::set<int>{}));
  EXPECT_EQ(Res.IN(1), (std::set<int>{1}));
  EXPECT_EQ(Res.IN(2), (std::set<int>{1, 2}));
  EXPECT_EQ(Res.IN(3), (std::set<int>{1, 2, 3}));
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
      return {};
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
      return 0;
    case 1:
      return 0;
    case 2:
      return 1;
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
  EXPECT_EQ(Res.IN(0), (std::set<int>{}));
  EXPECT_EQ(Res.IN(1), (std::set<int>{1, 2}));
  EXPECT_EQ(Res.IN(2), (std::set<int>{1, 2}));
  EXPECT_EQ(Res.IN(3), (std::set<int>{1, 2, 3}));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
