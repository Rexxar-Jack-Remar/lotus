#include "Dataflow/APA/Solver/Inter/Modular/Solver.h"

#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Fwd = dataflow::controlflow::FlowDirection;

// Reachability-style domain over int nodes: fact 1 = reachable, 0 = top.
//   main: 1(entry) -> 2(call id) -> 3(exit)
//   id:   11(entry) -> 12(exit)
struct FakeICF {
  bool isCallSite(int I) const { return I == 2; }
  std::vector<int> getAllInstructionsOf(int F) const {
    if (F == 100) return {1, 2, 3};
    if (F == 200) return {11, 12};
    return {};
  }
  std::vector<int> getSuccsOf(int I, Fwd) const {
    if (I == 1) return {2};
    if (I == 11) return {12};
    return {};
  }
  std::vector<int> getStartPointsOf(int F) const {
    if (F == 100) return {1};
    if (F == 200) return {11};
    return {};
  }
  std::vector<int> getExitPointsOf(int F) const {
    if (F == 100) return {3};
    if (F == 200) return {12};
    return {};
  }
  std::vector<int> getReturnSitesOfCallAt(int C) const {
    return C == 2 ? std::vector<int>{3} : std::vector<int>{};
  }
  std::vector<int> getCalleesOfCallAt(int C) const {
    return C == 2 ? std::vector<int>{200} : std::vector<int>{};
  }
};

struct FakeDomain {
  using n_t = int; using fact_t = int; using transfer_t = int;
  using f_t = int; using i_t = FakeICF;
  using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
};

class FakeProblem : public elimination::InterEliminationProblem<FakeDomain> {
public:
  FakeProblem()
      : elimination::InterEliminationProblem<FakeDomain>({100}, nullptr) {}
  fact_t normalFlow(n_t, const fact_t &In) override { return In; }
  fact_t join(const fact_t &L, const fact_t &R) const override {
    return L > R ? L : R; // reachability: OR
  }
  bool equal(const fact_t &L, const fact_t &R) const override {
    return L == R;
  }
  fact_t bottom() const override { return 0; }
  n_t transferSuccessor(const transfer_t &) const override { return 0; }
  std::unordered_map<n_t, fact_t> initialSeeds() override { return {}; }
  // callFlow/returnFlow just propagate reachability across the boundary.
  fact_t callFlow(n_t, f_t, const fact_t &In) override { return In; }
  fact_t returnFlow(n_t, f_t, n_t, n_t, const fact_t &In) override {
    return In;
  }
  fact_t callToRetFlow(n_t, n_t, const std::vector<f_t> &,
                       const fact_t &In) override {
    return In;
  }
  std::vector<f_t> getCalleesOfCallAt(n_t C) const override {
    return C == 2 ? std::vector<f_t>{200} : std::vector<f_t>{};
  }
};

using Driver = elimination::ModularInterSummaryDriver<FakeDomain, 2>;
using Context = elimination::InterDataFlowResultT<2, int, int, int>::Context;

TEST(ModularInterSummaryDriver, ReachabilityPropagatesThroughCall) {
  FakeICF ICF;
  FakeProblem Problem;
  Driver D(Problem, ICF);
  auto R = D.solve({100}, /*InitialFact=*/1);

  const Context Empty{};
  // main entry and exit reachable.
  ASSERT_NE(R.tryIN(1, Empty), nullptr);
  EXPECT_EQ(*R.tryIN(1, Empty), 1);
  EXPECT_EQ(*R.tryIN(3, Empty), 1);
  // callee id reachable through the SummaryCall/callFlow path.
  ASSERT_NE(R.tryIN(11, Empty), nullptr);
  EXPECT_EQ(*R.tryIN(11, Empty), 1);
  EXPECT_EQ(*R.tryIN(12, Empty), 1);
}

// D4: enabling per-procedure EAN must not change the interpreted facts. EAN is
// a semantics-preserving optimization of the summary batch, so reachability is
// identical to the EAN-off run above.
TEST(ModularInterSummaryDriver, PerProcedureEANPreservesFacts) {
  FakeICF ICF;
  FakeProblem Problem;
  elimination::PathSummaryEquationOptions Opts;
  Opts.EAN.EnableEAN = true;
  Driver D(Problem, ICF, Opts);
  auto R = D.solve({100}, /*InitialFact=*/1);

  const Context Empty{};
  ASSERT_NE(R.tryIN(1, Empty), nullptr);
  EXPECT_EQ(*R.tryIN(1, Empty), 1);
  EXPECT_EQ(*R.tryIN(3, Empty), 1);
  ASSERT_NE(R.tryIN(11, Empty), nullptr);
  EXPECT_EQ(*R.tryIN(11, Empty), 1);
  EXPECT_EQ(*R.tryIN(12, Empty), 1);
}

TEST(ModularInterSummaryDriver, RecursionTerminatesAndReachesFixpoint) {
  // Self-recursive: main -> main. Fixpoint must terminate; entry reachable.
  struct RecICF {
    bool isCallSite(int I) const { return I == 2; }
    std::vector<int> getAllInstructionsOf(int F) const {
      return F == 100 ? std::vector<int>{1, 2, 3} : std::vector<int>{};
    }
    std::vector<int> getSuccsOf(int I, Fwd) const {
      return I == 1 ? std::vector<int>{2} : std::vector<int>{};
    }
    std::vector<int> getStartPointsOf(int F) const {
      return F == 100 ? std::vector<int>{1} : std::vector<int>{};
    }
    std::vector<int> getExitPointsOf(int F) const {
      return F == 100 ? std::vector<int>{3} : std::vector<int>{};
    }
    std::vector<int> getReturnSitesOfCallAt(int C) const {
      return C == 2 ? std::vector<int>{3} : std::vector<int>{};
    }
    std::vector<int> getCalleesOfCallAt(int C) const {
      return C == 2 ? std::vector<int>{100} : std::vector<int>{};
    }
  };
  struct RecDomain {
    using n_t = int; using fact_t = int; using transfer_t = int;
    using f_t = int; using i_t = RecICF;
    using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
  };
  class RecProblem : public elimination::InterEliminationProblem<RecDomain> {
  public:
    RecProblem()
        : elimination::InterEliminationProblem<RecDomain>({100}, nullptr) {}
    fact_t normalFlow(n_t, const fact_t &In) override { return In; }
    fact_t join(const fact_t &L, const fact_t &R) const override {
      return L > R ? L : R;
    }
    bool equal(const fact_t &L, const fact_t &R) const override {
      return L == R;
    }
    fact_t bottom() const override { return 0; }
    n_t transferSuccessor(const transfer_t &) const override { return 0; }
    std::unordered_map<n_t, fact_t> initialSeeds() override { return {}; }
    fact_t callFlow(n_t, f_t, const fact_t &In) override { return In; }
    fact_t returnFlow(n_t, f_t, n_t, n_t, const fact_t &In) override {
      return In;
    }
    fact_t callToRetFlow(n_t, n_t, const std::vector<f_t> &,
                         const fact_t &In) override {
      return In;
    }
    std::vector<f_t> getCalleesOfCallAt(n_t C) const override {
      return C == 2 ? std::vector<f_t>{100} : std::vector<f_t>{};
    }
  };

  RecICF ICF;
  RecProblem Problem;
  elimination::ModularInterSummaryDriver<RecDomain, 2> D(Problem, ICF);
  auto R = D.solve({100}, 1); // must terminate

  const elimination::InterDataFlowResultT<2, int, int, int>::Context Empty{};
  ASSERT_NE(R.tryIN(1, Empty), nullptr);
  EXPECT_EQ(*R.tryIN(1, Empty), 1);
}

// Regression: reachability must flow the full depth of a call chain
//   main(100) -> A(200) -> B(300) -> C(400)
// The deepest callee C is reached only after entry facts propagate three hops.
// Because the driver processes callees before callers (one hop per pass), the
// outer fixpoint must NOT stop while entry facts are still moving. Watching only
// the interpreter's recursion-memo "changed" flag broke here: C stayed
// unreachable (mirrors the dirname main->version_etc->version_etc_va->
// version_etc_arn under-approximation).
TEST(ModularInterSummaryDriver, ReachabilityFlowsDeepCallChain) {
  struct ChainICF {
    bool isCallSite(int I) const { return I == 2 || I == 12 || I == 22; }
    std::vector<int> getAllInstructionsOf(int F) const {
      if (F == 100) return {1, 2, 3};
      if (F == 200) return {11, 12, 13};
      if (F == 300) return {21, 22, 23};
      if (F == 400) return {31, 32};
      return {};
    }
    std::vector<int> getSuccsOf(int I, Fwd) const {
      if (I == 1) return {2};
      if (I == 2) return {3};
      if (I == 11) return {12};
      if (I == 12) return {13};
      if (I == 21) return {22};
      if (I == 22) return {23};
      if (I == 31) return {32};
      return {};
    }
    std::vector<int> getStartPointsOf(int F) const {
      if (F == 100) return {1};
      if (F == 200) return {11};
      if (F == 300) return {21};
      if (F == 400) return {31};
      return {};
    }
    std::vector<int> getExitPointsOf(int F) const {
      if (F == 100) return {3};
      if (F == 200) return {13};
      if (F == 300) return {23};
      if (F == 400) return {32};
      return {};
    }
    std::vector<int> getReturnSitesOfCallAt(int C) const {
      if (C == 2) return {3};
      if (C == 12) return {13};
      if (C == 22) return {23};
      return {};
    }
    std::vector<int> getCalleesOfCallAt(int C) const {
      if (C == 2) return {200};
      if (C == 12) return {300};
      if (C == 22) return {400};
      return {};
    }
  };
  struct ChainDomain {
    using n_t = int; using fact_t = int; using transfer_t = int;
    using f_t = int; using i_t = ChainICF;
    using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
  };
  class ChainProblem : public elimination::InterEliminationProblem<ChainDomain> {
  public:
    ChainProblem()
        : elimination::InterEliminationProblem<ChainDomain>({100}, nullptr) {}
    fact_t normalFlow(n_t, const fact_t &In) override { return In; }
    fact_t join(const fact_t &L, const fact_t &R) const override {
      return L > R ? L : R;
    }
    bool equal(const fact_t &L, const fact_t &R) const override {
      return L == R;
    }
    fact_t bottom() const override { return 0; }
    n_t transferSuccessor(const transfer_t &) const override { return 0; }
    std::unordered_map<n_t, fact_t> initialSeeds() override { return {}; }
    fact_t callFlow(n_t, f_t, const fact_t &In) override { return In; }
    fact_t returnFlow(n_t, f_t, n_t, n_t, const fact_t &In) override {
      return In;
    }
    fact_t callToRetFlow(n_t, n_t, const std::vector<f_t> &,
                         const fact_t &In) override {
      return In;
    }
    std::vector<f_t> getCalleesOfCallAt(n_t C) const override {
      if (C == 2) return {200};
      if (C == 12) return {300};
      if (C == 22) return {400};
      return {};
    }
  };

  ChainICF ICF;
  ChainProblem Problem;
  elimination::ModularInterSummaryDriver<ChainDomain, 2> D(Problem, ICF);
  auto R = D.solve({100}, 1);

  const elimination::InterDataFlowResultT<2, int, int, int>::Context Empty{};
  // Every level, including the deepest callee C, must be reachable.
  ASSERT_NE(R.tryIN(11, Empty), nullptr);
  EXPECT_EQ(*R.tryIN(11, Empty), 1); // A
  ASSERT_NE(R.tryIN(21, Empty), nullptr);
  EXPECT_EQ(*R.tryIN(21, Empty), 1); // B
  ASSERT_NE(R.tryIN(31, Empty), nullptr);
  EXPECT_EQ(*R.tryIN(31, Empty), 1); // C (the deepest hop)
  EXPECT_EQ(*R.tryIN(32, Empty), 1);
}

} // namespace
