// EAN M6 tests: wiring EAN into the intraprocedural APA solver.
//
// Verifies (RQ1 correctness) that enabling EAN does not change any client fact:
//  * distributive domain (reachability) with the full Kleene profile — results
//    identical, and the retained path-expression DAG shrinks;
//  * non-distributive domain (a mod-2 constant lattice) with the universally
//    safe default profile (left distributivity only) — results identical.
// It also documents WHY the default is safe-minimal: enabling right
// distributivity on the non-distributive domain would change the result.
//
// LLVM-free: uses toy AnalysisDomains directly on the generic solver.

#include "Dataflow/APA/Solver/Intra/IntraSolver.h"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using elimination::EliminationOptions;
using elimination::IntraEliminationProblem;
using elimination::IntraEliminationSolver;
namespace ean = elimination::ean;

using Ref = elimination::PathExprFactory<int>::Ref;

void collectExpr(const Ref &e, std::unordered_set<const void *> &seen) {
  if (!e || !seen.insert(e.get()).second) return;
  if (e->L) collectExpr(e->L, seen);
  if (e->R) collectExpr(e->R, seen);
}
template <typename ResT> std::size_t uniqueExprNodes(const ResT &r) {
  std::unordered_set<const void *> seen;
  for (const auto &N : r.nodes()) collectExpr(r.ExprTo(N), seen);
  return seen.size();
}

// ---- distributive reachability domain (meet = set union) -------------------
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
    for (auto &kv : Succs) Ns.push_back(kv.first);
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

// ---- non-distributive mod-2 constant lattice -------------------------------
// fact: BOT (no info) < constant < TOP (conflict). transfer >=0 sets that
// constant; transfer -1 maps a value to (value % 2). mod-2 is many-to-one, so
// it does NOT distribute over meet (e.g. mod2(2 ⊔ 4) = mod2(TOP) = TOP, but
// mod2(2) ⊔ mod2(4) = 0 ⊔ 0 = 0).
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
    for (auto &kv : Succs) Ns.push_back(kv.first);
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
    if (T >= 0) return T; // set constant
    if (In == BOT) return BOT;
    if (In == TOP) return TOP;
    return In % 2; // mod2
  }
  fact_t join(const fact_t &a, const fact_t &b) const override {
    if (a == BOT) return b;
    if (b == BOT) return a;
    if (a == TOP || b == TOP) return TOP;
    return a == b ? a : TOP;
  }
  bool equal(const fact_t &a, const fact_t &b) const override { return a == b; }
  fact_t bottom() const override { return BOT; }
  fact_t initialFact() const override { return BOT; }

private:
  std::unordered_map<int, std::vector<int>> Succs;
  std::map<std::pair<int, int>, int> Edge;
};

} // namespace

// Distributive client + full profile: results identical, DAG no larger.
TEST(EanWiring, DistributiveResultsUnchangedAndSmaller) {
  // diamond: 0 -> {1,2} -> 3 ; edges into 3 share the same atom (Dst=3).
  std::unordered_map<int, std::vector<int>> S = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}};

  ReachProblem P0(S);
  IntraEliminationSolver<ReachDomain> Off(P0);
  Off.solve();
  const auto &ROff = Off.getResults();

  EliminationOptions Opts;
  Opts.EnableEAN = true;
  Opts.EANLaws = ean::LawProfile::kleeneAlgebra(); // reachability is distributive
  ReachProblem P1(S);
  IntraEliminationSolver<ReachDomain> On(P1, Opts);
  On.solve();
  const auto &ROn = On.getResults();

  for (int n : {0, 1, 2, 3}) {
    const auto *a = ROff.tryIN(n);
    const auto *b = ROn.tryIN(n);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(*a, *b) << "node " << n;
  }
  EXPECT_LE(uniqueExprNodes(ROn), uniqueExprNodes(ROff));
}

// Non-distributive client + safe-minimal default profile: results identical.
TEST(EanWiring, NonDistributiveSafeMinimalPreservesResults) {
  std::unordered_map<int, std::vector<int>> S = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}};
  std::map<std::pair<int, int>, int> E = {
      {{0, 1}, 2}, {{0, 2}, 4}, {{1, 3}, -1}, {{2, 3}, -1}}; // set2/set4 then mod2

  Mod2Problem P0(S, E);
  IntraEliminationSolver<Mod2Domain> Off(P0);
  Off.solve();
  const auto &ROff = Off.getResults();

  EliminationOptions Opts;
  Opts.EnableEAN = true; // default EANLaws = safeMinimal() (left distributivity)
  Mod2Problem P1(S, E);
  IntraEliminationSolver<Mod2Domain> On(P1, Opts);
  On.solve();
  const auto &ROn = On.getResults();

  ASSERT_NE(ROff.tryIN(3), nullptr);
  EXPECT_EQ(*ROff.tryIN(3), 0); // mod2(2)=mod2(4)=0, meet=0
  for (int n : {0, 1, 2, 3}) {
    ASSERT_NE(ROn.tryIN(n), nullptr);
    EXPECT_EQ(*ROn.tryIN(n), *ROff.tryIN(n)) << "node " << n;
  }
}

// Documents why the default is safe-minimal: right distributivity is UNSOUND on
// this non-distributive client and changes the result.
TEST(EanWiring, RightDistributivityUnsoundOnNonDistributive) {
  std::unordered_map<int, std::vector<int>> S = {
      {0, {1, 2}}, {1, {3}}, {2, {3}}, {3, {}}};
  std::map<std::pair<int, int>, int> E = {
      {{0, 1}, 2}, {{0, 2}, 4}, {{1, 3}, -1}, {{2, 3}, -1}};

  EliminationOptions Opts;
  Opts.EnableEAN = true;
  Opts.EANLaws = ean::LawProfile::kleeneAlgebra(); // WRONG for this client
  Mod2Problem P(S, E);
  IntraEliminationSolver<Mod2Domain> On(P, Opts);
  On.solve();
  const auto &R = On.getResults();

  ASSERT_NE(R.tryIN(3), nullptr);
  // (set2 ⊕ set4)·mod2 = mod2(meet(2,4)=TOP) = TOP, vs the correct 0.
  EXPECT_EQ(*R.tryIN(3), TOP);
}
