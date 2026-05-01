/**
 * @file EliminationTest.cpp
 * @brief Unit tests for elimination-based (state elimination) dataflow solver
 */

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Analyses/LLVM/Inter/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/LLVM/Inter/LiveVariables.h"
#include "Dataflow/APA/Analyses/LLVM/Inter/Reachability.h"
#include "Dataflow/APA/Analyses/LLVM/Inter/ReachingDefinitions.h"
#include "Dataflow/APA/Analyses/LLVM/Inter/UninitializedVariables.h"
#include "Dataflow/APA/Analyses/LLVM/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/LLVM/Intra/LiveVariables.h"
#include "Dataflow/APA/Analyses/LLVM/Intra/Reachability.h"
#include "TestUtils/LLVMHelpers.h"

#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

class APATest : public ::testing::Test {
protected:
  llvm::LLVMContext Context;

  llvm::Instruction *findInstructionByName(llvm::Function *F,
                                           llvm::StringRef Name) {
    return lotus::unittest::findInstructionByName(F, Name);
  }

  template <typename InstT> InstT *findFirst(llvm::Function *F) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *Match = llvm::dyn_cast<InstT>(&I)) {
          return Match;
        }
      }
    }
    return nullptr;
  }
};

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

TEST_F(APATest, LLVMReachabilitySkipsUnreachableBlock) {
  const char *Source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %live = add i32 1, 2
      br label %exit
    else:
      br label %exit
    dead:
      %deadv = add i32 40, 2
      br label %exit
    exit:
      %phi = phi i32 [ %live, %then ], [ 0, %else ]
      ret i32 %phi
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimReachable(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Live = findInstructionByName(F, "live");
  auto *Dead = findInstructionByName(F, "deadv");
  auto *Ret = findFirst<llvm::ReturnInst>(F);
  ASSERT_NE(Live, nullptr);
  ASSERT_NE(Dead, nullptr);
  ASSERT_NE(Ret, nullptr);

  ASSERT_NE(Result.tryIN(Live), nullptr);
  EXPECT_TRUE(*Result.tryIN(Live));
  ASSERT_NE(Result.tryIN(Ret), nullptr);
  EXPECT_TRUE(*Result.tryIN(Ret));
  EXPECT_EQ(Result.tryIN(Dead), nullptr);
}

TEST_F(APATest, LLVMConstantPropagationTracksFoldedValuesAtReturn) {
  const char *Source = R"(
    define i32 @test() {
    entry:
      %sum = add i32 1, 2
      %scaled = mul i32 %sum, 4
      ret i32 %scaled
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimConstantPropagation(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Scaled = findInstructionByName(F, "scaled");
  auto *Ret = findFirst<llvm::ReturnInst>(F);
  ASSERT_NE(Scaled, nullptr);
  ASSERT_NE(Ret, nullptr);

  auto *Facts = Result.tryIN(Ret);
  ASSERT_NE(Facts, nullptr);

  auto ScaledIt = Facts->find(Scaled);
  ASSERT_NE(ScaledIt, Facts->end());
}

TEST_F(APATest, LLVMLiveVariablesMergesFactsAcrossMultipleReturns) {
  const char *Source = R"(
    define i32 @test(i1 %cond, i32 %a, i32 %b) {
    entry:
      %sum = add i32 %a, %b
      br i1 %cond, label %ret_sum, label %ret_a
    ret_sum:
      ret i32 %sum
    ret_a:
      ret i32 %a
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimLiveVariables(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Sum = findInstructionByName(F, "sum");
  auto *A = F->getArg(1);
  auto *B = F->getArg(2);
  ASSERT_NE(Sum, nullptr);
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);

  auto BBIt = F->begin();
  ++BBIt; // ret_sum
  auto *RetSumInst = llvm::cast<llvm::ReturnInst>(BBIt->getTerminator());
  auto *RetAInst = llvm::cast<llvm::ReturnInst>(F->back().getTerminator());

  auto *RetSumFacts = Result.tryIN(RetSumInst);
  auto *RetAFacts = Result.tryIN(RetAInst);
  ASSERT_NE(RetSumFacts, nullptr);
  ASSERT_NE(RetAFacts, nullptr);
  EXPECT_NE(RetSumFacts->find(Sum), RetSumFacts->end());
  EXPECT_EQ(RetSumFacts->find(A), RetSumFacts->end());
  EXPECT_NE(RetAFacts->find(A), RetAFacts->end());
  EXPECT_EQ(RetAFacts->find(B), RetAFacts->end());
}

TEST_F(APATest, InterproceduralReachabilityDirectCallAndDeadFunction) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      %sum = add i32 %x, 0
      ret i32 %sum
    }

    define i32 @dead() {
    entry:
      %unused = add i32 7, 8
      ret i32 %unused
    }

    define i32 @main() {
    entry:
      %call = call i32 @id(i32 3)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  auto *Dead = Module->getFunction("dead");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);
  ASSERT_NE(Dead, nullptr);

  auto Result = elimination::runInterElimReachable(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeEntry = &*Id->getEntryBlock().begin();
  auto *CalleeRet = findFirst<llvm::ReturnInst>(Id);
  auto *DeadInst = findInstructionByName(Dead, "unused");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);
  ASSERT_NE(CalleeRet, nullptr);
  ASSERT_NE(DeadInst, nullptr);

  EXPECT_NE(Result.tryIN(Call, {}), nullptr);
  EXPECT_NE(Result.tryIN(MainRet, {}), nullptr);
  EXPECT_NE(Result.tryIN(CalleeEntry, {Call}), nullptr);
  EXPECT_NE(Result.tryIN(CalleeRet, {Call}), nullptr);
  EXPECT_EQ(Result.tryIN(DeadInst, {}), nullptr);
}

TEST_F(APATest, InterproceduralConstantPropagationReturnThroughIdentity) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @dead() {
    entry:
      %unused = add i32 9, 9
      ret i32 %unused
    }

    define i32 @main() {
    entry:
      %seed = add i32 1, 2
      %call = call i32 @id(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  auto *Dead = Module->getFunction("dead");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);
  ASSERT_NE(Dead, nullptr);

  auto Result = elimination::runInterElimConstantPropagation(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *DeadInst = findInstructionByName(Dead, "unused");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(DeadInst, nullptr);
  auto *CallerFacts = Result.tryIN(MainRet, {});
  ASSERT_NE(CallerFacts, nullptr);
  auto It = CallerFacts->find(Call);
  ASSERT_NE(It, CallerFacts->end());
  if (It->second.isConstant()) {
    auto *CI = llvm::dyn_cast<llvm::ConstantInt>(It->second.getConstant());
    ASSERT_NE(CI, nullptr);
    EXPECT_EQ(CI->getSExtValue(), 3);
  }

  auto *CalleeFacts = Result.tryIN(&*Id->getEntryBlock().begin(), {Call});
  ASSERT_NE(CalleeFacts, nullptr);
  auto FormalIt = CalleeFacts->find(&*Id->arg_begin());
  ASSERT_NE(FormalIt, CalleeFacts->end());
  if (FormalIt->second.isConstant()) {
    auto *FormalCI =
        llvm::dyn_cast<llvm::ConstantInt>(FormalIt->second.getConstant());
    ASSERT_NE(FormalCI, nullptr);
    EXPECT_EQ(FormalCI->getSExtValue(), 3);
  }

  EXPECT_FALSE(It->second.isUnknown());
  EXPECT_FALSE(FormalIt->second.isUnknown());

  EXPECT_EQ(Result.tryIN(DeadInst, {}), nullptr);
}

TEST_F(APATest, InterproceduralLiveVariablesBackwardAcrossCall) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %seed = add i32 1, 2
      %unused = add i32 %seed, 5
      %call = call i32 @id(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);

  auto Result = elimination::runInterElimLiveVariables(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Seed = findInstructionByName(Main, "seed");
  auto *Unused = findInstructionByName(Main, "unused");
  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeRet = findFirst<llvm::ReturnInst>(Id);
  ASSERT_NE(Seed, nullptr);
  ASSERT_NE(Unused, nullptr);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeRet, nullptr);

  auto *CallFacts = Result.tryIN(Call, {});
  ASSERT_NE(CallFacts, nullptr);
  EXPECT_NE(CallFacts->find(Seed), CallFacts->end());
  EXPECT_EQ(CallFacts->find(Unused), CallFacts->end());

  auto *CalleeFacts = Result.tryOUT(CalleeRet, {Call});
  ASSERT_NE(CalleeFacts, nullptr);
  EXPECT_NE(CalleeFacts->find(&*Id->arg_begin()), CalleeFacts->end());

  auto *RetFacts = Result.tryOUT(MainRet, {});
  ASSERT_NE(RetFacts, nullptr);
  EXPECT_NE(RetFacts->find(Call), RetFacts->end());
}

TEST_F(APATest, InterproceduralConstantPropagationThroughPointerArgument) {
  const char *Source = R"(
    define void @set42(i32* %p) {
    entry:
      store i32 42, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i32
      store i32 7, i32* %slot
      call void @set42(i32* %slot)
      %loaded = load i32, i32* %slot
      ret i32 %loaded
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto Result = elimination::runInterElimConstantPropagation(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Slot = findInstructionByName(Main, "slot");
  auto *Load = findInstructionByName(Main, "loaded");
  auto *Ret = findFirst<llvm::ReturnInst>(Main);
  ASSERT_NE(Slot, nullptr);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(Ret, nullptr);

  auto *Facts = Result.tryIN(Ret, {});
  ASSERT_NE(Facts, nullptr);
  auto SlotIt = Facts->find(Slot);
  ASSERT_NE(SlotIt, Facts->end());
  EXPECT_FALSE(SlotIt->second.isUnknown());

  auto LoadIt = Facts->find(Load);
  ASSERT_NE(LoadIt, Facts->end());
  EXPECT_FALSE(LoadIt->second.isUnknown());
}

TEST_F(APATest, InterproceduralReachingDefinitionsAcrossCall) {
  const char *Source = R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %seed = add i32 1, 2
      %call = call i32 @id(i32 %seed)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *Id = Module->getFunction("id");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Id, nullptr);

  auto Result = elimination::runInterElimReachingDefinitions(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *MainRet = findFirst<llvm::ReturnInst>(Main);
  auto *CalleeEntry = &*Id->getEntryBlock().begin();
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(MainRet, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);

  auto *CalleeFacts = Result.tryIN(CalleeEntry, {Call});
  ASSERT_NE(CalleeFacts, nullptr);
  EXPECT_NE(CalleeFacts->find(&*Id->arg_begin()), CalleeFacts->end());

  auto *RetFacts = Result.tryIN(MainRet, {});
  ASSERT_NE(RetFacts, nullptr);
  EXPECT_NE(RetFacts->find(Call), RetFacts->end());
}

TEST_F(APATest, InterproceduralUninitializedVariablesAcrossCall) {
  const char *Source = R"(
    define i32 @loadit(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %slot = alloca i32
      %call = call i32 @loadit(i32* %slot)
      ret i32 %call
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *Main = Module->getFunction("main");
  auto *LoadIt = Module->getFunction("loadit");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(LoadIt, nullptr);

  auto Result = elimination::runInterElimUninitVariables(Main);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Call = findFirst<llvm::CallInst>(Main);
  auto *CalleeEntry = &*LoadIt->getEntryBlock().begin();
  auto *Load = findInstructionByName(LoadIt, "v");
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(CalleeEntry, nullptr);
  ASSERT_NE(Load, nullptr);

  auto *CalleeFacts = Result.tryIN(CalleeEntry, {Call});
  ASSERT_NE(CalleeFacts, nullptr);
  EXPECT_NE(CalleeFacts->find(&*LoadIt->arg_begin()), CalleeFacts->end());

  auto *LoadFacts = Result.tryOUT(Load, {Call});
  ASSERT_NE(LoadFacts, nullptr);
  EXPECT_NE(LoadFacts->find(Load), LoadFacts->end());
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
