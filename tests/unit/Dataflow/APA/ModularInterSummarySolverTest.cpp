#include "Dataflow/APA/Solver/Inter/Modular/SummaryBuilder.h"

#include <functional>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Fwd = dataflow::controlflow::FlowDirection;

// Two-procedure program:
//   main:  1(entry) -> 2(call id) -> 3(exit)
//   id:    11(entry) -> 12(exit)
// Procedure ids: main=100, id=200. Instruction 0 is reserved as the null node.
struct FakeICF {
  bool isCallSite(int Inst) const { return Inst == 2; }

  std::vector<int> getAllInstructionsOf(int F) const {
    if (F == 100) return {1, 2, 3};
    if (F == 200) return {11, 12};
    return {};
  }

  std::vector<int> getSuccsOf(int Inst, Fwd) const {
    switch (Inst) {
    case 1: return {2};
    case 11: return {12};
    default: return {}; // 2 is a call site (handled via return sites), 3/12 exits
    }
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
  std::vector<int> getReturnSitesOfCallAt(int CallSite) const {
    return CallSite == 2 ? std::vector<int>{3} : std::vector<int>{};
  }
  std::vector<int> getCalleesOfCallAt(int CallSite) const {
    return CallSite == 2 ? std::vector<int>{200} : std::vector<int>{};
  }
};

struct FakeDomain {
  using n_t = int;
  using fact_t = int;
  using transfer_t = int;
  using f_t = int;
  using i_t = FakeICF;
  using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
};

class FakeProblem : public elimination::InterEliminationProblem<FakeDomain> {
public:
  FakeProblem() : elimination::InterEliminationProblem<FakeDomain>({100}, nullptr) {}
  fact_t normalFlow(n_t, const fact_t &In) override { return In; }
  fact_t join(const fact_t &L, const fact_t &R) const override {
    return L > R ? L : R;
  }
  bool equal(const fact_t &L, const fact_t &R) const override {
    return L == R;
  }
  // transfer_t == n_t: edgeTransfer returns Src (forward); keep default.
  n_t transferSuccessor(const transfer_t &) const override { return 0; }
  std::unordered_map<n_t, fact_t> initialSeeds() override { return {}; }
  fact_t callFlow(n_t, f_t, const fact_t &In) override { return In; }
  fact_t returnFlow(n_t, f_t, n_t, n_t, const fact_t &In) override { return In; }
  fact_t callToRetFlow(n_t, n_t, const std::vector<f_t> &,
                       const fact_t &In) override {
    return In;
  }
  std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const override {
    return CallSite == 2 ? std::vector<f_t>{200} : std::vector<f_t>{};
  }
};

using Builder = elimination::ModularInterSummaryBuilder<FakeDomain>;
using Atom = elimination::InterSummaryTransferAtom<FakeDomain>;
using ExprRef = elimination::PathExprFactory<Atom>::Ref;

// True iff the expression DAG contains a SummaryCall atom for the given callee.
bool containsSummaryCall(const ExprRef &E, int Callee) {
  if (!E) return false;
  using Kind = elimination::PathExprFactory<Atom>::Kind;
  switch (E->K) {
  case Kind::Atom:
    return E->Transfer->K == Atom::Kind::SummaryCall &&
           E->Transfer->Callee == Callee;
  case Kind::Union:
  case Kind::Concat:
    return containsSummaryCall(E->L, Callee) ||
           containsSummaryCall(E->R, Callee);
  case Kind::Star:
    return containsSummaryCall(E->L, Callee);
  default:
    return false;
  }
}

TEST(ModularInterSummary, BuildsPerProcedureSummaries) {
  FakeICF ICF;
  FakeProblem Problem;
  Builder B(Problem, ICF);
  auto R = B.build({100});

  // Both procedures got a summary; callee before caller in reverse-topo order.
  ASSERT_EQ(R.summaries.size(), 2u);
  ASSERT_TRUE(R.summaries.count(100));
  ASSERT_TRUE(R.summaries.count(200));
  ASSERT_EQ(R.callGraph.order.size(), 2u);
  EXPECT_EQ(R.callGraph.order.front().procs.front(), 200); // id first (leaf)
  EXPECT_EQ(R.callGraph.order.back().procs.front(), 100);  // main last

  // main's entry->exit summary references id symbolically (not inlined).
  const auto &Main = R.summaries.at(100);
  ASSERT_TRUE(Main.exit != nullptr);
  EXPECT_TRUE(containsSummaryCall(Main.exit, /*Callee=*/200));

  // id's summary is a leaf: no SummaryCall atoms.
  const auto &Id = R.summaries.at(200);
  ASSERT_TRUE(Id.exit != nullptr);
  EXPECT_FALSE(containsSummaryCall(Id.exit, 200));

  // Per-node coverage: every instruction of each procedure has an expression.
  EXPECT_EQ(Main.perNode.size(), 3u);
  EXPECT_EQ(Id.perNode.size(), 2u);
}

TEST(ModularInterSummary, RecursionIsSymbolicNotInlined) {
  // A self-recursive procedure: its summary references itself via SummaryCall,
  // and construction terminates (no inlining, no infinite expansion).
  struct RecICF {
    bool isCallSite(int Inst) const { return Inst == 2; }
    std::vector<int> getAllInstructionsOf(int F) const {
      return F == 100 ? std::vector<int>{1, 2, 3} : std::vector<int>{};
    }
    std::vector<int> getSuccsOf(int Inst, Fwd) const {
      return Inst == 1 ? std::vector<int>{2} : std::vector<int>{};
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
      return C == 2 ? std::vector<int>{100} : std::vector<int>{}; // calls self
    }
  };
  struct RecDomain {
    using n_t = int; using fact_t = int; using transfer_t = int;
    using f_t = int; using i_t = RecICF;
    using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
  };
  class RecProblem : public elimination::InterEliminationProblem<RecDomain> {
  public:
    RecProblem() : elimination::InterEliminationProblem<RecDomain>({100}, nullptr) {}
    fact_t normalFlow(n_t, const fact_t &In) override { return In; }
    fact_t join(const fact_t &L, const fact_t &R) const override { return L > R ? L : R; }
    bool equal(const fact_t &L, const fact_t &R) const override { return L == R; }
    n_t transferSuccessor(const transfer_t &) const override { return 0; }
    std::unordered_map<n_t, fact_t> initialSeeds() override { return {}; }
    fact_t callFlow(n_t, f_t, const fact_t &In) override { return In; }
    fact_t returnFlow(n_t, f_t, n_t, n_t, const fact_t &In) override { return In; }
    fact_t callToRetFlow(n_t, n_t, const std::vector<f_t> &, const fact_t &In) override { return In; }
    std::vector<f_t> getCalleesOfCallAt(n_t C) const override {
      return C == 2 ? std::vector<f_t>{100} : std::vector<f_t>{};
    }
  };

  RecICF ICF;
  RecProblem Problem;
  elimination::ModularInterSummaryBuilder<RecDomain> B(Problem, ICF);
  auto R = B.build({100});

  ASSERT_EQ(R.summaries.size(), 1u);
  ASSERT_EQ(R.callGraph.order.size(), 1u);
  EXPECT_TRUE(R.callGraph.order.front().recursive);
  using RAtom = elimination::InterSummaryTransferAtom<RecDomain>;
  const auto &S = R.summaries.at(100);
  ASSERT_TRUE(S.exit != nullptr);
  // Self-reference present as a symbolic SummaryCall to procedure 100.
  std::function<bool(const elimination::PathExprFactory<RAtom>::Ref &)> hasSelf =
      [&](const elimination::PathExprFactory<RAtom>::Ref &E) -> bool {
    using Kind = elimination::PathExprFactory<RAtom>::Kind;
    if (!E) return false;
    if (E->K == Kind::Atom)
      return E->Transfer->K == RAtom::Kind::SummaryCall &&
             E->Transfer->Callee == 100;
    if (E->K == Kind::Star) return hasSelf(E->L);
    return hasSelf(E->L) || hasSelf(E->R);
  };
  EXPECT_TRUE(hasSelf(S.exit));
}

} // namespace
