// Tests for cost-aware elimination ordering (paper's "Order" configuration).
//
// Two layers:
//   * Combinatorial — the pure elimination-graph model in EliminationOrder.h:
//     greedy minimum-product beats the baseline on a hub graph, is a valid
//     permutation, and is deterministic.
//   * Wiring/correctness — through the real solver on LLVM-free toy domains:
//     on a distributive client the pivot order does NOT change any client fact
//     (Floyd–Warshall closure is order-invariant); the non-distributive case
//     is documented as an inherent APA caveat (like the EAN law profile).

#include "Dataflow/APA/Solver/Intra/IntraSolver.h"
#include "Dataflow/APA/Solver/Ordering/StructuralModel.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using elimination::EliminationOptions;
using elimination::IntraEliminationProblem;
using elimination::IntraEliminationSolver;
using elimination::OrderingPolicy;
namespace order = elimination::order;

// ------------------------------------------------------------ combinatorial --

// Hub: sources 1..P -> hub 0 -> sinks P+1..P+Q. Eliminating the hub first costs
// P*Q updates and P*Q fill edges; eliminating the (product-0) leaves first
// costs nothing.
order::EliminationGraph makeHub(std::size_t P, std::size_t Q) {
  const std::size_t n = 1 + P + Q;
  order::EliminationGraph g(n);
  for (std::size_t i = 1; i <= P; ++i)
    g.addEdge(i, 0);
  for (std::size_t j = 0; j < Q; ++j)
    g.addEdge(0, 1 + P + j);
  return g;
}

bool isPermutation(const std::vector<std::size_t> &o, std::size_t n) {
  if (o.size() != n)
    return false;
  std::vector<std::size_t> s = o;
  std::sort(s.begin(), s.end());
  for (std::size_t i = 0; i < n; ++i)
    if (s[i] != i)
      return false;
  return true;
}

std::vector<std::size_t> identityOrder(std::size_t n) {
  std::vector<std::size_t> o(n);
  std::iota(o.begin(), o.end(), std::size_t{0});
  return o;
}

TEST(EliminationOrderCombinatorial, CostAwareBeatsBaselineOnHub) {
  const std::size_t P = 5, Q = 5;
  auto g = makeHub(P, Q);
  auto costAware = order::computeCostAwareOrder(g);

  ASSERT_TRUE(isPermutation(costAware, g.n));

  const auto ca = order::simulateCost(g, costAware);
  const auto id = order::simulateCost(g, identityOrder(g.n));

  // Baseline (identity) eliminates the hub first: P*Q product and fill.
  EXPECT_DOUBLE_EQ(id.totalProduct, static_cast<double>(P * Q));
  // Cost-aware peels the product-0 leaves first: zero product, no fill.
  EXPECT_DOUBLE_EQ(ca.totalProduct, 0.0);
  EXPECT_LT(ca.totalProduct, id.totalProduct);
  EXPECT_LE(ca.peakFill, id.peakFill);
}

TEST(EliminationOrderCombinatorial, Deterministic) {
  auto g = makeHub(6, 4);
  EXPECT_EQ(order::computeCostAwareOrder(g), order::computeCostAwareOrder(g));
}

TEST(EliminationOrderCombinatorial, ChainNeverWorseThanIdentity) {
  // 0->1->2->3->4 chain.
  const std::size_t n = 5;
  order::EliminationGraph g(n);
  for (std::size_t i = 0; i + 1 < n; ++i)
    g.addEdge(i, i + 1);
  auto ca = order::computeCostAwareOrder(g);
  ASSERT_TRUE(isPermutation(ca, n));
  EXPECT_LE(order::simulateCost(g, ca).totalProduct,
            order::simulateCost(g, identityOrder(n)).totalProduct);
}

TEST(EliminationOrderCombinatorial, EmptyAndSingleton) {
  EXPECT_TRUE(order::computeCostAwareOrder(order::EliminationGraph(0)).empty());
  auto one = order::computeCostAwareOrder(order::EliminationGraph(1));
  ASSERT_EQ(one.size(), 1u);
  EXPECT_EQ(one[0], 0u);
}

// ------------------------------------------------------------ wiring domains
// --

// Distributive reachability domain (meet = set union): pivot order cannot
// change any client fact.
struct ReachDomain {
  using n_t = int;
  using fact_t = std::set<int>;
  using transfer_t = int;
  using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
};
class ReachProblem final : public IntraEliminationProblem<ReachDomain> {
public:
  explicit ReachProblem(std::unordered_map<int, std::vector<int>> S)
      : Succs(std::move(S)) {}
  std::vector<int> nodes() const override {
    std::vector<int> Ns;
    for (auto &kv : Succs)
      Ns.push_back(kv.first);
    std::sort(Ns.begin(), Ns.end());
    return Ns;
  }
  int entry() const override { return 0; }
  std::vector<int> succs(int n) const override {
    auto it = Succs.find(n);
    return it == Succs.end() ? std::vector<int>{} : it->second;
  }
  int edgeTransfer(int, int Dst) const override { return Dst; }
  fact_t applyTransfer(const int &T, const fact_t &In) const override {
    fact_t o = In;
    o.insert(T);
    return o;
  }
  fact_t join(const fact_t &a, const fact_t &b) const override {
    fact_t o = a;
    o.insert(b.begin(), b.end());
    return o;
  }
  bool equal(const fact_t &a, const fact_t &b) const override { return a == b; }
  fact_t bottom() const override { return {}; }
  fact_t initialFact() const override { return {}; }

private:
  std::unordered_map<int, std::vector<int>> Succs;
};

// A hub-shaped reachable CFG: entry 0 -> {1..P} -> hub H -> {sinks} -> exit.
std::unordered_map<int, std::vector<int>> hubCFG(int P, int Q) {
  std::unordered_map<int, std::vector<int>> S;
  const int H = 1 + P;         // hub id
  const int firstSink = H + 1; // sinks firstSink..firstSink+Q-1
  S[0] = {};
  for (int i = 1; i <= P; ++i) { // entry fans out to sources, sources -> hub
    S[0].push_back(i);
    S[i] = {H};
  }
  S[H] = {};
  for (int j = 0; j < Q; ++j) {
    const int sink = firstSink + j;
    S[H].push_back(sink);
    S[sink] = {};
  }
  return S;
}

TEST(OrderWiring, DistributivePivotOrderInvariant) {
  auto S = hubCFG(4, 4);

  ReachProblem P0(S);
  EliminationOptions Def; // Ordering = Default
  IntraEliminationSolver<ReachDomain> Baseline(P0, Def);
  Baseline.solve();
  const auto &RB = Baseline.getResults();

  ReachProblem P1(S);
  EliminationOptions Ord;
  Ord.Ordering = OrderingPolicy::CostAware;
  IntraEliminationSolver<ReachDomain> Cost(P1, Ord);
  Cost.solve();
  const auto &RC = Cost.getResults();

  for (int n : P0.nodes()) {
    const auto *a = RB.tryIN(n);
    const auto *b = RC.tryIN(n);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(*a, *b) << "node " << n;
  }
}

// Non-distributive mod-2 constant lattice — documents that pivot order (like
// the EAN law profile, and like the existing reverse-topo/identity choice) can
// in principle change a non-distributive client's result. We only require that
// CostAware still produces a valid solution; we do NOT assert equality with the
// baseline, because divergence here is inherent to APA, not a defect.
constexpr int BOT = -100;
constexpr int TOP = -200;
struct Mod2Domain {
  using n_t = int;
  using fact_t = int;
  using transfer_t = int;
  using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
};
class Mod2Problem final : public IntraEliminationProblem<Mod2Domain> {
public:
  Mod2Problem(std::unordered_map<int, std::vector<int>> S,
              std::map<std::pair<int, int>, int> E)
      : Succs(std::move(S)), Edge(std::move(E)) {}
  std::vector<int> nodes() const override {
    std::vector<int> Ns;
    for (auto &kv : Succs)
      Ns.push_back(kv.first);
    std::sort(Ns.begin(), Ns.end());
    return Ns;
  }
  int entry() const override { return 0; }
  std::vector<int> succs(int n) const override {
    auto it = Succs.find(n);
    return it == Succs.end() ? std::vector<int>{} : it->second;
  }
  int edgeTransfer(int Src, int Dst) const override {
    return Edge.at({Src, Dst});
  }
  fact_t applyTransfer(const int &T, const fact_t &In) const override {
    if (T >= 0)
      return T;
    if (In == BOT)
      return BOT;
    if (In == TOP)
      return TOP;
    return In % 2;
  }
  fact_t join(const fact_t &a, const fact_t &b) const override {
    if (a == BOT)
      return b;
    if (b == BOT)
      return a;
    if (a == TOP || b == TOP)
      return TOP;
    return a == b ? a : TOP;
  }
  bool equal(const fact_t &a, const fact_t &b) const override { return a == b; }
  fact_t bottom() const override { return BOT; }
  fact_t initialFact() const override { return BOT; }

private:
  std::unordered_map<int, std::vector<int>> Succs;
  std::map<std::pair<int, int>, int> Edge;
};

TEST(OrderWiring, NonDistributiveCostAwareStillSolves) {
  std::unordered_map<int, std::vector<int>> S = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}};
  std::map<std::pair<int, int>, int> E = {
      {{0, 1}, 2}, {{0, 2}, 4}, {{1, 3}, -1}, {{2, 3}, -1}};

  Mod2Problem P(S, E);
  EliminationOptions Ord;
  Ord.Ordering = OrderingPolicy::CostAware;
  IntraEliminationSolver<Mod2Domain> Cost(P, Ord);
  Cost.solve();
  const auto &R = Cost.getResults();
  // A valid solution exists for every node; exact value is order-dependent for
  // this non-distributive client and is therefore not asserted against Default.
  for (int n : {0, 1, 2, 3})
    EXPECT_NE(R.tryIN(n), nullptr) << "node " << n;
}

TEST(OrderWiring, OnlinePoliciesPreserveEveryPointIncludingUnreachableLoops) {
  const std::unordered_map<int, std::vector<int>> Edges = {
      {0, {0, 1, 2}}, {1, {2, 3}}, {2, {1, 3}}, {3, {}}, {4, {4}}};
  ReachProblem Problem(Edges);
  IntraEliminationSolver<ReachDomain> Baseline(Problem);
  ASSERT_EQ(Baseline.solve(), elimination::SolveStatus::Ok);
  for (auto Policy : {OrderingPolicy::Structural,
                      OrderingPolicy::ExpressionAware, OrderingPolicy::StarRisk,
                      OrderingPolicy::Hybrid, OrderingPolicy::ReversePostOrder,
                      OrderingPolicy::MinDegree, OrderingPolicy::Random}) {
    EliminationOptions Opts;
    Opts.Ordering = Policy;
    Opts.Order.RecordTrace = true;
    Opts.MeasurePeakNodes = true;
    IntraEliminationSolver<ReachDomain> Solver(Problem, Opts);
    ASSERT_EQ(Solver.solve(), elimination::SolveStatus::Ok);
    for (auto Node : Problem.nodes()) {
      ASSERT_NE(Solver.getResults().tryIN(Node), nullptr);
      EXPECT_EQ(*Solver.getResults().tryIN(Node),
                *Baseline.getResults().tryIN(Node));
      EXPECT_TRUE(Solver.getResults().ExprTo(Node) != nullptr);
    }
    EXPECT_EQ(Solver.getDiagnostics().ordering.trace.size(),
              Problem.nodes().size());
    EXPECT_GT(Solver.getDiagnostics().peak_matrix_nodes, 0u);
    EXPECT_GT(Solver.getDiagnostics().semantic_star_time_ns, 0u);
  }
}

TEST(OrderWiring, SparseBaselineAndLegacyOrderPreserveFacts) {
  ReachProblem Problem(hubCFG(3, 3));
  IntraEliminationSolver<ReachDomain> Baseline(Problem);
  Baseline.solve();
  for (auto Policy : {OrderingPolicy::Default, OrderingPolicy::CostAware}) {
    EliminationOptions Opts;
    Opts.Ordering = Policy;
    Opts.Order.UseSparseElimination = true;
    IntraEliminationSolver<ReachDomain> Solver(Problem, Opts);
    ASSERT_EQ(Solver.solve(), elimination::SolveStatus::Ok);
    for (auto Node : Problem.nodes()) {
      EXPECT_EQ(*Solver.getResults().tryIN(Node),
                *Baseline.getResults().tryIN(Node));
    }
    EXPECT_EQ(Solver.getDiagnostics().ordering.selected_nodes,
              Problem.nodes().size());
  }
}

TEST(OrderWiring, IncompatibleADTAndOnlineOrderingIsRejected) {
  ReachProblem Problem({{0, {1}}, {1, {}}});
  EliminationOptions Opts;
  Opts.Method = elimination::EliminationMethod::ADTSimple;
  Opts.Ordering = OrderingPolicy::Hybrid;
  IntraEliminationSolver<ReachDomain> Solver(Problem, Opts);
  ASSERT_EQ(Solver.solve(), elimination::SolveStatus::InvalidProblem);
  EXPECT_FALSE(Solver.usedADT());
  EXPECT_EQ(Solver.getDiagnostics().executed_method,
            elimination::EliminationMethod::StateElimination);
}

TEST(OrderWiring, InvalidOnlineOptionsReportInvalidProblem) {
  ReachProblem Problem({{0, {1}}, {1, {}}});
  EliminationOptions Opts;
  Opts.Ordering = OrderingPolicy::Explicit;
  Opts.Order.ExplicitOrder = {0, 0};
  IntraEliminationSolver<ReachDomain> Solver(Problem, Opts);
  EXPECT_EQ(Solver.solve(), elimination::SolveStatus::InvalidProblem);
  EXPECT_TRUE(Solver.getResults().nodes().empty());
}

} // namespace
