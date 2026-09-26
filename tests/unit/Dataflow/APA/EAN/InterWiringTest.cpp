// Interprocedural EAN/Greedy wiring tests (M6b).
//
// ForwardInterSummarySolver builds one path-expression summary per
// (instruction, call-string context) and interprets it into a client fact.
// EAN/Greedy post-optimize that summary batch before interpretation. These
// tests assert:
//   (a) the optimized forms produce IDENTICAL IN/OUT facts (RQ1-inter:
//       safe-minimal EAN/Greedy are semantics-preserving for every client);
//   (b) they never enlarge the retained DAG, and strictly shrink it on a batch
//       with a shared prefix (Table VI-inter).
//
// The summary interpreter composes atoms as Concat = sequential apply and
// Union = merge, so left distributivity (a·b)⊕(a·c) = a·(b⊕c) holds
// unconditionally — the same structural argument as the intraprocedural case —
// regardless of atom kind. A join-closed set-domain client is used so Star
// iterations converge and the distributive-shaped algebra is exercised.

#include "Dataflow/APA/Core/InterProblem.h"
#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/Solver/Inter/ExpandedSolver.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using dataflow::controlflow::FlowDirection;

// A minimal fake interprocedural CFG over integer node ids. Purely
// intraprocedural edges (no call sites) suffice to exercise the summary
// path-expression batch: EAN treats every atom as opaque, so atom KIND is
// irrelevant to the factorization being validated.
struct FakeICF {
  std::unordered_map<int, std::vector<int>> Succ; // forward successors
  std::vector<int> Starts;                         // entry instruction(s)

  bool isCallSite(int) const { return false; }
  std::vector<int> getCalleesOfCallAt(int) const { return {}; }
  std::vector<int> getStartPointsOf(int) const { return Starts; }
  std::vector<int> getExitPointsOf(int) const { return {}; }
  std::vector<int> getReturnSitesOfCallAt(int) const { return {}; }
  std::vector<int> getSuccsOf(int Inst, FlowDirection) const {
    auto It = Succ.find(Inst);
    return It == Succ.end() ? std::vector<int>{} : It->second;
  }
};

struct FakeDomain {
  using n_t = int;
  using fact_t = std::uint64_t; // small bitset lattice
  using transfer_t = int;       // transfer == source node (default edgeTransfer)
  using f_t = int;
  using i_t = FakeICF;
  using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
};

// normalFlow(node, in) = in | bit(node); merge = OR. A monotone, join-closed
// set domain: Star iterations converge, and Union/Concat exercise the
// distributive-shaped algebra the factorization relies on.
class FakeProblem : public elimination::InterEliminationProblem<FakeDomain> {
public:
  FakeProblem(const FakeICF *ICF, int Entry)
      : elimination::InterEliminationProblem<FakeDomain>({Entry}, ICF),
        EntryInst(Entry) {}

  static std::uint64_t bit(int Node) {
    return std::uint64_t{1} << (static_cast<unsigned>(Node) % 63u);
  }

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    return In | bit(Inst);
  }
  fact_t join(const fact_t &A, const fact_t &B) const override { return A | B; }
  bool equal(const fact_t &A, const fact_t &B) const override {
    return A == B;
  }
  fact_t bottom() const override { return 0; }

  fact_t callFlow(n_t, f_t, const fact_t &In) override { return In; }
  fact_t returnFlow(n_t, f_t, n_t, n_t, const fact_t &In) override { return In; }
  fact_t callToRetFlow(n_t, n_t, const std::vector<f_t> &,
                       const fact_t &In) override {
    return In;
  }
  std::unordered_map<n_t, fact_t> initialSeeds() override {
    return {{EntryInst, bit(EntryInst)}};
  }
  std::vector<f_t> getCalleesOfCallAt(n_t) const override { return {}; }

private:
  int EntryInst;
};

constexpr unsigned kK = 2;
using SolverTy = elimination::ForwardInterSummarySolver<FakeDomain, kK>;

struct SolveOut {
  std::map<int, std::uint64_t> In;
  std::map<int, std::uint64_t> Out;
  elimination::InterSummarySolveDiagnostics Diag;
};

// Run the solver over `Icf` with the given post-pass options and collect
// IN/OUT facts keyed by instruction id (contexts are empty for the pure
// intraprocedural fake, so there is exactly one key per node).
SolveOut runSolve(const FakeICF &Icf, int Entry, const std::vector<int> &Nodes,
                  const elimination::InterEANOptions &EanOpts) {
  FakeProblem Problem(&Icf, Entry);
  elimination::PathSummaryEquationOptions Opts;
  Opts.EAN = EanOpts;
  SolverTy Solver(Problem, Opts);
  Solver.solve();

  SolveOut Out;
  Out.Diag = Solver.resultDiagnostics();
  const auto *Res = Solver.getResults();
  EXPECT_NE(Res, nullptr);
  if (Res == nullptr) {
    return Out;
  }
  for (int N : Nodes) {
    const auto Keys = Res->contextsForInstruction(N);
    for (const auto &Key : Keys) {
      if (const auto *InFact = Res->tryIN(Key)) {
        Out.In[N] = *InFact;
      }
      if (const auto *OutFact = Res->tryOUT(Key)) {
        Out.Out[N] = *OutFact;
      }
    }
  }
  return Out;
}

elimination::InterEANOptions defaultOpts() { return {}; }

elimination::InterEANOptions eanOpts() {
  elimination::InterEANOptions O;
  O.EnableEAN = true;
  O.EANMonotone = true; // never enlarge the batch (Table VI-inter guarantee)
  return O;
}

elimination::InterEANOptions greedyOpts() {
  elimination::InterEANOptions O;
  O.EnableGreedy = true;
  return O;
}

// entry 1 fans out 3-way to {2,3,4} (shared prefix), merges at 5 with a
// self-loop (Star), then exits at 6. Exercises Union, Concat, Star, and a
// factorable shared prefix in one graph.
FakeICF fanLoopCfg() {
  FakeICF Icf;
  Icf.Starts = {1};
  Icf.Succ[1] = {2, 3, 4};
  Icf.Succ[2] = {5};
  Icf.Succ[3] = {5};
  Icf.Succ[4] = {5};
  Icf.Succ[5] = {5, 6}; // self-loop + exit
  return Icf;
}
const std::vector<int> fanLoopNodes = {1, 2, 3, 4, 5, 6};

TEST(InterEAN, EANPreservesSemantics) {
  const FakeICF Icf = fanLoopCfg();
  const auto Base = runSolve(Icf, 1, fanLoopNodes, defaultOpts());
  const auto Ean = runSolve(Icf, 1, fanLoopNodes, eanOpts());
  EXPECT_EQ(Base.In, Ean.In);
  EXPECT_EQ(Base.Out, Ean.Out);
}

TEST(InterEAN, GreedyPreservesSemantics) {
  const FakeICF Icf = fanLoopCfg();
  const auto Base = runSolve(Icf, 1, fanLoopNodes, defaultOpts());
  const auto Greedy = runSolve(Icf, 1, fanLoopNodes, greedyOpts());
  EXPECT_EQ(Base.In, Greedy.In);
  EXPECT_EQ(Base.Out, Greedy.Out);
}

// The monotone guard must ensure the retained summary DAG never grows, on
// either the EAN or the Greedy post-pass. (Whether it STRICTLY shrinks depends
// on the batch: on small solver outputs EAN's re-binarized canonical form is
// often no smaller than the solver's native factoring, and the guard then
// returns the input verbatim — an honest, batch-dependent outcome. Strict
// reduction on the inter atom algebra is validated deterministically below in
// EANFactorsInterAtomBatch, and empirically on the corpus.)
TEST(InterEAN, NeverEnlargesBatch) {
  const FakeICF Icf = fanLoopCfg();
  const auto Ean = runSolve(Icf, 1, fanLoopNodes, eanOpts());
  ASSERT_GT(Ean.Diag.summary_before.uniqueNodes, 0u);
  EXPECT_LE(Ean.Diag.summary_after.uniqueNodes,
            Ean.Diag.summary_before.uniqueNodes);

  const auto Greedy = runSolve(Icf, 1, fanLoopNodes, greedyOpts());
  EXPECT_LE(Greedy.Diag.summary_after.uniqueNodes,
            Greedy.Diag.summary_before.uniqueNodes);
}

// Deterministic proof that EAN reduces a batch over the interprocedural atom
// algebra: (a·b) ⊕ (a·c) has a shared prefix `a` that left distributivity
// factors to a·(b⊕c), removing one concat node. Run unguarded so the guard
// cannot mask the reduction. This also exercises ean<atom_t> — the generic
// instantiation over the opaque inter transfer atom.
TEST(InterEAN, EANFactorsInterAtomBatch) {
  using Atom = elimination::InterSummaryTransferAtom<FakeDomain>;
  using Factory = elimination::PathExprFactory<Atom>;
  Factory F;
  const auto A = F.atom(Atom::rawNormal(1));
  const auto B = F.atom(Atom::rawNormal(2));
  const auto C = F.atom(Atom::rawNormal(3));
  std::vector<Factory::Ref> R = {F.unite(F.concat(A, B), F.concat(A, C))};

  const auto Before = elimination::ean::computeDagStats<Atom>(R).uniqueNodes;
  elimination::ean::ExtractOptions EO; // monotoneGuard = false
  const auto Out = elimination::ean::ean<Atom>(
      R, elimination::ean::LawProfile::safeMinimal(),
      elimination::ean::CostModel::uniform(),
      elimination::ean::Budget::unbounded(), F, nullptr, EO);
  ASSERT_EQ(Out.size(), 1u);
  const auto After = elimination::ean::computeDagStats<Atom>(Out).uniqueNodes;
  EXPECT_LT(After, Before) << "left-distributive prefix factoring should shrink";
}

// The default (no post-pass) path records the batch size but leaves it
// unchanged and adds no normalization cost.
TEST(InterEAN, DefaultIsNoOp) {
  const FakeICF Icf = fanLoopCfg();
  const auto Base = runSolve(Icf, 1, fanLoopNodes, defaultOpts());
  EXPECT_EQ(Base.Diag.summary_after.uniqueNodes,
            Base.Diag.summary_before.uniqueNodes);
  EXPECT_EQ(Base.Diag.norm_time_us, 0u);
}

// Randomized differential: on many random rooted DAGs (optionally with
// self-loops), EAN must never change any client fact.
std::uint64_t g_seed = 0xEA9;
std::uint32_t rnd() {
  g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
  return static_cast<std::uint32_t>(g_seed >> 33);
}

TEST(InterEAN, RandomizedDifferential) {
  for (int Trial = 0; Trial < 300; ++Trial) {
    const int N = 5 + static_cast<int>(rnd() % 6); // 5..10 nodes
    FakeICF Icf;
    Icf.Starts = {1};
    std::vector<int> Nodes;
    Nodes.reserve(N);
    for (int I = 1; I <= N; ++I) {
      Nodes.push_back(I);
    }
    // Each node i>1 gets 1-2 forward preds from {1..i-1}: rooted, reachable,
    // acyclic before optional self-loops.
    for (int I = 2; I <= N; ++I) {
      const int P1 = 1 + static_cast<int>(rnd() % static_cast<unsigned>(I - 1));
      Icf.Succ[P1].push_back(I);
      if (rnd() % 2 == 0) {
        const int P2 =
            1 + static_cast<int>(rnd() % static_cast<unsigned>(I - 1));
        if (P2 != P1) {
          Icf.Succ[P2].push_back(I);
        }
      }
      if (rnd() % 4 == 0) {
        Icf.Succ[I].push_back(I); // self-loop -> Star
      }
    }

    const auto Base = runSolve(Icf, 1, Nodes, defaultOpts());
    const auto Ean = runSolve(Icf, 1, Nodes, eanOpts());
    EXPECT_EQ(Base.In, Ean.In) << "trial " << Trial;
    EXPECT_EQ(Base.Out, Ean.Out) << "trial " << Trial;
    EXPECT_LE(Ean.Diag.summary_after.uniqueNodes,
              Ean.Diag.summary_before.uniqueNodes)
        << "trial " << Trial;
  }
}

} // namespace
