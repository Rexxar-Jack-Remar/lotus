// EAN evaluation — RQ3 (complementarity with elimination ordering, paper §IV-D,
// Table VI Order/Order+EAN rows). LLVM-free: synthetic CFG families run through
// the REAL intraprocedural solver with a distributive reachability client, in
// the 2x2 {Default, Order} x {no-EAN, EAN} configuration matrix.
//
// For every subject and configuration we:
//   * assert client-fact parity vs Default (pivot order + EAN preserve results
//     on a distributive client) — the RQ3 correctness net;
//   * measure final retained-DAG complexity over the summary batch (Table VI);
//   * measure peak construction nodes (Diagnostics.peak_matrix_nodes) — the
//     ordering-only metric, plus the combinatorial peakFill proxy.
// It then reports the Order/EAN/Order+EAN reduction rows, the Default-vs-Order
// peak reduction per family, and the Spearman correlation between the ordering
// gain and the EAN gain (complementarity).

#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/Solver/Ordering/StructuralModel.h"
#include "Dataflow/APA/Solver/Intra/IntraSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using elimination::EliminationMethod;
using elimination::EliminationOptions;
using elimination::IntraEliminationProblem;
using elimination::IntraEliminationSolver;
using elimination::OrderingPolicy;
namespace ean = elimination::ean;
namespace order = elimination::order;

// ---- distributive reachability client (meet = set union) -------------------
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

using CFG = std::unordered_map<int, std::vector<int>>;

// ---------------------------------------------------------------- corpus ----
// Hub: entry 0 -> sources 1..P -> hub H -> sinks -> (dead). Ordering's主场:
// eliminating the hub first costs P*Q; peeling leaves first costs nothing.
CFG hub(int P, int Q) {
  CFG S;
  const int H = 1 + P, firstSink = H + 1;
  S[0] = {};
  for (int i = 1; i <= P; ++i) { S[0].push_back(i); S[i] = {H}; }
  S[H] = {};
  for (int j = 0; j < Q; ++j) { S[H].push_back(firstSink + j); S[firstSink + j] = {}; }
  return S;
}
// Dense complete DAG: i -> j for all i < j. High predecessor-successor product.
CFG dense(int m) {
  CFG S;
  for (int i = 0; i < m; ++i) {
    S[i] = {};
    for (int j = i + 1; j < m; ++j) S[i].push_back(j);
  }
  return S;
}
// Diamond chain: d segments, each entry_t -> {a_t,b_t} -> entry_{t+1}. Two
// parallel branches per segment create factorable shared prefixes/suffixes.
CFG diamondChain(int d) {
  CFG S;
  int next = 0;
  int entry = next++;
  S[entry] = {};
  for (int t = 0; t < d; ++t) {
    int a = next++, b = next++, join = next++;
    S[entry] = {a, b};
    S[a] = {join};
    S[b] = {join};
    S[join] = {};
    entry = join;
  }
  return S;
}
// Nested loops: a spine 0->1->...->L with a back edge from each node to node 1
// (nested self-similar cycles) -> star-rich summaries.
CFG nestedLoop(int L) {
  CFG S;
  for (int i = 0; i <= L; ++i) S[i] = {};
  for (int i = 0; i < L; ++i) S[i].push_back(i + 1);
  for (int i = 2; i <= L; ++i) S[i].push_back(1); // back edges to the loop head
  return S;
}
// Random reachable CFG (cycles allowed): spanning path 0->1->...->n-1 then
// extra random forward/back edges. Deterministic PRNG.
std::uint64_t g_seed = 0x3D3A11u;
std::uint32_t rnd() {
  g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
  return static_cast<std::uint32_t>(g_seed >> 33);
}
CFG randomCFG(int n, int extra) {
  CFG S;
  for (int i = 0; i < n; ++i) S[i] = {};
  for (int i = 0; i + 1 < n; ++i) S[i].push_back(i + 1); // spanning path
  for (int e = 0; e < extra; ++e) {
    int a = static_cast<int>(rnd() % n), b = static_cast<int>(rnd() % n);
    if (a == b) continue;
    S[a].push_back(b);
  }
  return S;
}

struct Subject { std::string family; std::string label; CFG cfg; };

void buildCorpus(std::vector<Subject> &out) {
  for (int P : {3, 6, 10}) for (int Q : {3, 6, 10})
    out.push_back({"Hub", "H" + std::to_string(P) + "x" + std::to_string(Q), hub(P, Q)});
  for (int m : {5, 7, 9, 11}) out.push_back({"Dense", "D" + std::to_string(m), dense(m)});
  for (int d : {2, 3, 4, 5}) out.push_back({"DiamondChain", "C" + std::to_string(d), diamondChain(d)});
  for (int L : {3, 4, 5, 6}) out.push_back({"NestedLoop", "N" + std::to_string(L), nestedLoop(L)});
  for (int t = 0; t < 12; ++t) {
    const int n = 6 + static_cast<int>(rnd() % 8);
    out.push_back({"Random", "R" + std::to_string(t), randomCFG(n, n)});
  }
}

// ---------------------------------------------------------------- helpers ---
struct GeoMean {
  double sumLog = 0.0; std::size_t n = 0;
  void add(double before, double after) {
    if (before > 0.0 && after > 0.0) { sumLog += std::log(after / before); ++n; }
  }
  double value() const { return n ? std::exp(sumLog / static_cast<double>(n)) : 1.0; }
};

std::string outDir() {
  const char *d = std::getenv("EAN_EVAL_OUT");
  return d && *d ? std::string(d) : std::string(".");
}
std::ofstream open(const std::string &name) { return std::ofstream(outDir() + "/" + name); }

EliminationOptions mkOpts(OrderingPolicy ord, bool enableEAN) {
  EliminationOptions o;
  o.Method = EliminationMethod::StateElimination;
  o.Ordering = ord;
  o.MeasurePeakNodes = true;
  o.EnableEAN = enableEAN;
  if (enableEAN) {
    o.EANLaws = ean::LawProfile::kleeneAlgebra(); // reachability is distributive
    o.EANCost = ean::CostModel::uniform();
    o.EANExtract.reuseIters = 3;
  }
  return o;
}

struct RunResult {
  ean::DagStats stats;
  std::size_t peakNodes = 0;
  std::map<int, std::set<int>> facts; // IN per node, for parity
};
RunResult run(const CFG &cfg, OrderingPolicy ord, bool enableEAN) {
  ReachProblem P(cfg);
  IntraEliminationSolver<ReachDomain> Solver(P, mkOpts(ord, enableEAN));
  Solver.solve();
  const auto &R = Solver.getResults();
  RunResult rr;
  std::vector<elimination::PathExprFactory<int>::Ref> batch;
  for (int n : P.nodes()) {
    auto E = R.ExprTo(n);
    if (E) batch.push_back(E);
    if (const auto *f = R.tryIN(n)) rr.facts[n] = *f;
  }
  rr.stats = ean::computeDagStats<int>(batch);
  rr.peakNodes = Solver.getDiagnostics().peak_matrix_nodes;
  return rr;
}

// Combinatorial peakFill proxy on the CFG (node ids -> sorted index).
std::pair<std::size_t, std::size_t> peakFillProxy(const CFG &cfg) {
  std::vector<int> ids;
  for (auto &kv : cfg) ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end());
  std::unordered_map<int, std::size_t> idx;
  for (std::size_t i = 0; i < ids.size(); ++i) idx[ids[i]] = i;
  order::EliminationGraph g(ids.size());
  for (auto &kv : cfg)
    for (int d : kv.second)
      if (idx.count(d)) g.addEdge(idx[kv.first], idx[d]);
  std::vector<std::size_t> identity(ids.size());
  for (std::size_t i = 0; i < ids.size(); ++i) identity[i] = i;
  const auto def = order::simulateCost(g, identity);
  const auto ord = order::simulateCost(g, order::computeCostAwareOrder(g));
  return {def.peakFill, ord.peakFill};
}

// Average-rank Spearman correlation.
double spearman(const std::vector<double> &x, const std::vector<double> &y) {
  const std::size_t n = x.size();
  if (n < 2) return 0.0;
  auto ranks = [n](const std::vector<double> &v) {
    std::vector<std::size_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return v[a] < v[b]; });
    std::vector<double> r(n);
    for (std::size_t i = 0; i < n;) {
      std::size_t j = i;
      while (j < n && v[idx[j]] == v[idx[i]]) ++j;
      const double avg = (static_cast<double>(i) + static_cast<double>(j - 1)) / 2.0 + 1.0;
      for (std::size_t k = i; k < j; ++k) r[idx[k]] = avg;
      i = j;
    }
    return r;
  };
  const auto rx = ranks(x), ry = ranks(y);
  double mx = 0, my = 0;
  for (std::size_t i = 0; i < n; ++i) { mx += rx[i]; my += ry[i]; }
  mx /= n; my /= n;
  double num = 0, dx = 0, dy = 0;
  for (std::size_t i = 0; i < n; ++i) {
    num += (rx[i] - mx) * (ry[i] - my);
    dx += (rx[i] - mx) * (rx[i] - mx);
    dy += (ry[i] - my) * (ry[i] - my);
  }
  return (dx > 0 && dy > 0) ? num / std::sqrt(dx * dy) : 0.0;
}

} // namespace

TEST(EanEvalRq3, ComplementarityWithOrdering) {
  std::vector<Subject> corpus;
  buildCorpus(corpus);

  // Table VI rows (geo-mean of config/Default ratios).
  GeoMean gO_n, gO_e, gO_t, gO_s, gO_st;         // Order
  GeoMean gE_n, gE_e, gE_t, gE_s, gE_st;         // EAN
  GeoMean gOE_n, gOE_e, gOE_t, gOE_s, gOE_st;    // Order+EAN
  double shareDef = 0, shareO = 0, shareE = 0, shareOE = 0; std::size_t shareN = 0;

  // Per-family peak aggregation.
  struct PeakAgg { std::size_t subjects = 0, defPeak = 0, ordPeak = 0, defFill = 0, ordFill = 0; };
  std::map<std::string, PeakAgg> peak;
  std::vector<std::string> famOrder;

  // Complementarity gains + final-node geo-means.
  std::vector<double> orderingGain, eanGain;
  GeoMean gOrdVsDef, gEanVsDef, gOEVsDef, gOEVsOrd;

  std::size_t parityChecks = 0, parityFail = 0;

  for (auto &s : corpus) {
    auto D = run(s.cfg, OrderingPolicy::Default, false);
    auto O = run(s.cfg, OrderingPolicy::CostAware, false);
    auto E = run(s.cfg, OrderingPolicy::Default, true);
    auto OE = run(s.cfg, OrderingPolicy::CostAware, true);

    // parity: every config must agree with Default on every node's fact.
    for (auto &kv : D.facts) {
      ++parityChecks;
      if (O.facts[kv.first] != kv.second || E.facts[kv.first] != kv.second ||
          OE.facts[kv.first] != kv.second)
        ++parityFail;
    }

    const auto &d = D.stats;
    auto acc = [&](const ean::DagStats &c, GeoMean &n, GeoMean &e, GeoMean &t,
                   GeoMean &sq, GeoMean &st) {
      n.add(d.uniqueNodes, c.uniqueNodes); e.add(d.uniqueEdges, c.uniqueEdges);
      t.add(d.expandedTree, c.expandedTree); sq.add(d.concats, c.concats);
      st.add(d.stars, c.stars);
    };
    acc(O.stats, gO_n, gO_e, gO_t, gO_s, gO_st);
    acc(E.stats, gE_n, gE_e, gE_t, gE_s, gE_st);
    acc(OE.stats, gOE_n, gOE_e, gOE_t, gOE_s, gOE_st);
    shareDef += d.sharing(); shareO += O.stats.sharing();
    shareE += E.stats.sharing(); shareOE += OE.stats.sharing(); ++shareN;

    if (std::find(famOrder.begin(), famOrder.end(), s.family) == famOrder.end())
      famOrder.push_back(s.family);
    auto fill = peakFillProxy(s.cfg);
    PeakAgg &pa = peak[s.family];
    ++pa.subjects; pa.defPeak += D.peakNodes; pa.ordPeak += O.peakNodes;
    pa.defFill += fill.first; pa.ordFill += fill.second;

    orderingGain.push_back(static_cast<double>(d.uniqueNodes) - O.stats.uniqueNodes);
    eanGain.push_back(static_cast<double>(d.uniqueNodes) - E.stats.uniqueNodes);
    gOrdVsDef.add(d.uniqueNodes, O.stats.uniqueNodes);
    gEanVsDef.add(d.uniqueNodes, E.stats.uniqueNodes);
    gOEVsDef.add(d.uniqueNodes, OE.stats.uniqueNodes);
    gOEVsOrd.add(O.stats.uniqueNodes, OE.stats.uniqueNodes);
  }

  // ---- Table VI Order/EAN/Order+EAN rows ----
  {
    auto os = open("table6_order_rows.csv");
    os << "configuration,unique_nodes,dag_edges,tree_size,sequence,stars,"
          "sharing_before,sharing_after\n";
    auto row = [&](const char *name, GeoMean &n, GeoMean &e, GeoMean &t,
                   GeoMean &sq, GeoMean &st, double shareAfter) {
      os << name << "," << n.value() << "," << e.value() << "," << t.value()
         << "," << sq.value() << "," << st.value() << ","
         << (shareN ? shareDef / shareN : 0.0) << ","
         << (shareN ? shareAfter / shareN : 0.0) << "\n";
    };
    row("Order", gO_n, gO_e, gO_t, gO_s, gO_st, shareO);
    row("EAN", gE_n, gE_e, gE_t, gE_s, gE_st, shareE);
    row("Order+EAN", gOE_n, gOE_e, gOE_t, gOE_s, gOE_st, shareOE);
  }

  // ---- RQ3 peak (Default vs Order) ----
  {
    auto os = open("rq3_peak.csv");
    os << "family,subjects,default_peak_nodes,order_peak_nodes,peak_ratio,"
          "default_peakfill,order_peakfill\n";
    for (const auto &f : famOrder) {
      const PeakAgg &a = peak[f];
      const double dn = static_cast<double>(a.defPeak) / a.subjects;
      const double on = static_cast<double>(a.ordPeak) / a.subjects;
      os << f << "," << a.subjects << "," << dn << "," << on << ","
         << (dn > 0 ? on / dn : 1.0) << ","
         << static_cast<double>(a.defFill) / a.subjects << ","
         << static_cast<double>(a.ordFill) / a.subjects << "\n";
    }
  }

  // ---- RQ3 complementarity ----
  {
    auto os = open("rq3_complementarity.csv");
    os << "metric,value\n";
    os << "n_subjects," << corpus.size() << "\n";
    os << "spearman_ordering_vs_ean_gain," << spearman(orderingGain, eanGain) << "\n";
    os << "order_vs_default_final," << gOrdVsDef.value() << "\n";
    os << "ean_vs_default_final," << gEanVsDef.value() << "\n";
    os << "orderEAN_vs_default_final," << gOEVsDef.value() << "\n";
    os << "orderEAN_vs_order_final," << gOEVsOrd.value() << "\n";
    os << "parity_checks," << parityChecks << "\n";
    os << "parity_failures," << parityFail << "\n";
  }

  EXPECT_EQ(parityFail, 0u);
  std::printf("[RQ3] %zu subjects, parity %zu/%zu ok, Order/Def=%.3f EAN/Def=%.3f "
              "Order+EAN/Def=%.3f rho=%.3f\n",
              corpus.size(), parityChecks - parityFail, parityChecks,
              gOrdVsDef.value(), gEanVsDef.value(), gOEVsDef.value(),
              spearman(orderingGain, eanGain));
}
