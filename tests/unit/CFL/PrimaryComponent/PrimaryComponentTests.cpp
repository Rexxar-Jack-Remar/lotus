// SPDX-License-Identifier: MIT
#include "CFL/DynamicDyck/PrimaryComponent/PrimaryConnectivity.h"
#include "TestSupport.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <utility>

namespace {
using namespace test;
using Primary =
    lotus::cfl::dynamic_dyck::primary_component::PrimaryConnectivity;

void regressions() {
  { // Orientation: two CLOSING edges from one source merge their targets.
    Fixture f;
    f.insert(close(0, 1));
    f.insert(close(0, 2));
    f.expect({{0}, {1, 2}});
    CHECK(!f.solver.connected(0, 1));
  }
  { // Conversely two OPENING edges with a common source do not do so.
    Fixture f;
    f.insert(open(0, 1));
    f.insert(open(0, 2));
    f.expect({{0}, {1}, {2}});
  }
  { // Deleted-last-outgoing ghost summary, followed by an unrelated insertion.
    Fixture f;
    f.insert(close(0, 1));
    f.erase(close(0, 1));
    f.insert(close(0, 2));
    f.expect({{0}, {1}, {2}});
    f.insert(close(0, 3));
    f.expect({{0}, {1}, {2, 3}});
  }
  { // Count is semantic multiplicity, not multiple primary-chain vertices.
    Fixture f;
    f.insert(close(0, 1));
    f.insert(open(1, 0));
    f.insert(close(0, 2));
    const auto before = f.solver.diagnostics();
    f.erase(close(0, 1));
    CHECK(f.solver.connected(1, 2));
    CHECK(f.solver.diagnostics().make_primary_calls ==
          before.make_primary_calls);
    f.erase(open(1, 0));
    CHECK(!f.solver.connected(1, 2));
    f.erase(close(0, 1));
  }
  { // Two DIFFERENT chains can support the same undirected primary edge.
    Fixture f;
    f.insert(close(0, 2, 7));
    f.insert(close(0, 3, 7));
    f.insert(close(1, 2, 99));
    f.insert(close(1, 3, 99));
    CHECK(f.solver.diagnostics().primal_edges == 1);
    f.erase(close(0, 2, 7));
    CHECK(f.solver.connected(2, 3));
    f.erase(close(1, 2, 99));
    CHECK(!f.solver.connected(2, 3));
  }
  { // Head, interior, tail and singleton deletion; tail-witness migration.
    Fixture f;
    for (Vertex v = 1; v <= 6; ++v)
      f.insert(close(0, v, 23));
    for (Vertex v : {6, 3, 1, 4, 2, 5})
      f.erase(close(0, v, 23));
    CHECK(f.solver.diagnostics().primal_edges == 0);
    f.insert(close(0, 7, 23));
    f.expect({{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}});
  }
  { // Mutually supporting DSCCs must not survive loss of the grounding witness.
    Fixture f;
    for (Edge e : {close(0, 1, 0), close(0, 2, 0), close(1, 3, 1),
                   close(2, 4, 1), close(3, 1, 2), close(4, 2, 2)})
      f.insert(e);
    f.expect({{0}, {1, 2}, {3, 4}});
    f.erase(close(0, 2, 0));
    f.expect({{0}, {1}, {2}, {3}, {4}});
    f.insert(close(0, 2, 0));
    f.expect({{0}, {1, 2}, {3, 4}});
  }
  { // Self-loop and source-in-target-set cases of Algorithm 1, lines 22-25.
    Fixture f;
    for (Edge e : {close(0, 0, 0), close(0, 1, 0), close(1, 2, 0),
                   close(2, 3, 1), close(0, 4, 1), close(4, 4, 1)})
      f.insert(e);
    f.erase(close(0, 0, 0));
    f.erase(close(4, 4, 1));
    f.insert(close(2, 2, 0));
  }
  { // Distinct targets, not edge multiplicity or number of sources.
    Fixture f;
    for (Edge e :
         {close(0, 1, 0), close(0, 2, 0), close(1, 3, 1), close(2, 3, 1),
          close(1, 3, 1), close(4, 3, 2), close(4, 5, 2)})
      f.insert(e);
    const auto before = f.solver.diagnostics();
    f.erase(close(0, 2, 0));
    CHECK(f.solver.diagnostics().affected_vertices - before.affected_vertices ==
          2);
    CHECK(f.solver.connected(3, 5));
  }
  { // Figure 1 / Section 3.4, page 3: c,d,e,f,g,h = 0,1,2,3,4,5.
    Fixture f;
    for (Edge e : {close(0, 4, 1), close(3, 0, 0), close(3, 1, 0),
                   close(3, 2, 0), close(4, 2, 0), close(5, 3, 0)})
      f.insert(e);
    f.expect({{0, 1, 2}, {3}, {4}, {5}});
    f.insert(close(1, 5, 1));
    f.expect({{0, 1, 2, 3}, {4, 5}});
    f.erase(close(3, 1, 0));
    f.expect({{0, 2}, {1}, {3}, {4}, {5}});
  }
  { // Figure 2, page 8: u,x,y,z,v,w = 0,1,2,3,4,5.
    Fixture f;
    for (Edge e : {close(0, 1, 0), close(0, 2, 0), close(3, 2, 1),
                   close(3, 4, 1), close(1, 3, 2), close(4, 5, 2)})
      f.insert(e);
    f.expect({{0}, {1, 2, 4}, {3, 5}});
  }
  { // Figure 6, page 14: a,b,c,d,e,f,g,h,u,v,x,y = 0,...,11.
    // Includes an unaffected DSCC, affected-to-unaffected outgoing summaries,
    // and a PDSCC pair which must remerge after MakePrimary's refinement.
    Fixture f;
    for (Edge e :
         {close(0, 0, 0), close(0, 2, 0), close(2, 0, 1), close(2, 1, 1),
          close(2, 3, 2), close(2, 4, 2), close(5, 2, 2), close(3, 6, 0),
          close(6, 3, 2), close(4, 7, 0), close(3, 8, 1), close(5, 10, 1),
          close(8, 4, 2), close(9, 5, 2), close(11, 8, 0), close(11, 11, 0),
          close(11, 8, 1), close(11, 9, 1)})
      f.insert(e);
    f.expect({{0, 1, 2}, {3, 4, 5}, {6, 7}, {8, 9, 10, 11}});
    const auto before = f.solver.diagnostics();
    f.erase(close(2, 3, 2));
    f.expect({{0, 1, 2}, {3}, {4, 5}, {6}, {7}, {8, 9, 11}, {10}});
    CHECK(f.solver.diagnostics().affected_vertices - before.affected_vertices ==
          9);
    CHECK(f.solver.diagnostics().affected_components -
              before.affected_components ==
          3);
    CHECK(f.solver.primaryRepresentative(4) !=
          f.solver.primaryRepresentative(5));
    CHECK(f.solver.connected(4, 5));
  }
}

void api() {
  Fixture f;
  CHECK(f.solver.connectivityBackend() == test_backend);
  throws<std::invalid_argument>([] {
    PrimaryComponentSolver invalid(
        PrimaryComponentEdgeSemantics::Set,
        static_cast<PrimaryComponentConnectivityBackend>(44));
  });
  throws<std::out_of_range>([&] { f.solver.vertexIndex(0); });
  throws<std::out_of_range>([&] { f.solver.vertexAt(0); });
  throws<std::out_of_range>([&] { f.solver.connectedByIndex(0, 0); });
  throws<std::out_of_range>([&] { f.solver.representativeByIndex(0); });
  throws<std::out_of_range>([&] { f.solver.primaryRepresentativeByIndex(0); });
  verify(f.solver, f.oracle);
  CHECK(!f.solver.connected(999, 999));
  CHECK(!f.solver.deleteEdge(close(999, 1000)));
  CHECK(f.solver.statistics().vertices == 0);
  throws<std::out_of_range>([&] { f.solver.representative(999); });
  throws<std::out_of_range>([&] { f.solver.primaryRepresentative(999); });
  f.vertex(std::numeric_limits<Vertex>::min());
  f.vertex(std::numeric_limits<Vertex>::max());
  f.vertex(0);
  f.vertex(0);
  f.insert(close(std::numeric_limits<Vertex>::min(), 0,
                 std::numeric_limits<Label>::max()));
  f.insert(close(std::numeric_limits<Vertex>::min(),
                 std::numeric_limits<Vertex>::max(),
                 std::numeric_limits<Label>::max()));
  CHECK(f.solver.connected(0, std::numeric_limits<Vertex>::max()));
  const auto before = f.solver.components();
  throws<std::invalid_argument>([&] {
    f.solver.insertEdge({991, 992, 0, static_cast<Parenthesis>(44)});
  });
  throws<std::invalid_argument>([&] {
    f.solver.deleteEdge({991, 992, 0, static_cast<Parenthesis>(44)});
  });
  throws<std::invalid_argument>([&] {
    f.solver.edgeMultiplicity({991, 992, 0, static_cast<Parenthesis>(44)});
  });
  throws<std::invalid_argument>(
      [&] { f.solver.apply({static_cast<UpdateKind>(44), close(991, 992)}); });
  CHECK(f.solver.components() == before);
  CHECK(!f.solver.connected(991, 991));
  throws<std::invalid_argument>([] {
    PrimaryComponentSolver invalid(
        static_cast<PrimaryComponentEdgeSemantics>(44));
  });

  PrimaryComponentSolver moved(std::move(f.solver));
  verify(moved, f.oracle);
  throws<std::logic_error>([&] { f.solver.connected(0, 0); });
  f.solver = PrimaryComponentSolver{
      PrimaryComponentEdgeSemantics::ReferenceCounted, test_backend};
  CHECK(f.solver.statistics().vertices == 0);
  PrimaryComponentSolver assigned(
      PrimaryComponentEdgeSemantics::ReferenceCounted, test_backend);
  assigned.insertEdge(close(73, 74));
  assigned = std::move(moved);
  verify(assigned, f.oracle);
  CHECK(!assigned.connected(73, 73));
  throws<std::logic_error>([&] { moved.statistics(); });

  Graph g{{-10, 42, 99},
          {close(-10, 42, 7), open(42, -10, 7), close(-10, 99, 7)}};
  PrimaryComponentSolver counted(
      g, PrimaryComponentEdgeSemantics::ReferenceCounted, test_backend);
  PrimaryComponentSolver set(g, PrimaryComponentEdgeSemantics::Set,
                             test_backend);
  CHECK(counted.edgeMultiplicity(close(-10, 42, 7)) == 2);
  CHECK(set.edgeMultiplicity(close(-10, 42, 7)) == 1);
  CHECK(counted.deleteEdge(open(42, -10, 7)));
  CHECK(set.deleteEdge(open(42, -10, 7)));
  CHECK(counted.connected(42, 99));
  CHECK(!set.connected(42, 99));
  CHECK(!set.deleteEdge(open(42, -10, 7)));
  CHECK(set.insertEdge(open(42, -10, 7)));
  CHECK(!set.insertEdge(close(-10, 42, 7)));
  CHECK(set.edgeSemantics() == PrimaryComponentEdgeSemantics::Set);
  CHECK(counted.edgeSemantics() ==
        PrimaryComponentEdgeSemantics::ReferenceCounted);
  const Graph snapshot = counted.graph();
  CHECK(snapshot.vertices == std::vector<Vertex>({-10, 42, 99}));
  CHECK(snapshot.edges.size() == 2);
  PrimaryComponentSolver roundtrip(
      snapshot, PrimaryComponentEdgeSemantics::ReferenceCounted, test_backend);
  CHECK(roundtrip.components() == counted.components());
  CHECK(roundtrip.apply({UpdateKind::Delete, close(-10, 99, 7)}));
  CHECK(roundtrip.apply({UpdateKind::Insert, close(-10, 99, 7)}));
  CHECK(roundtrip.components() == counted.components());
  std::string error;
  CHECK(roundtrip.validate(&error));
  CHECK(error.empty());
}

void exhaustive() {
  for (auto dimensions : {std::pair<unsigned, unsigned>{3, 1}, {2, 2}}) {
    const unsigned n = dimensions.first, k = dimensions.second;
    std::vector<Edge> edges;
    for (unsigned u = 0; u < n; ++u)
      for (unsigned v = 0; v < n; ++v)
        for (unsigned label = 0; label < k; ++label)
          edges.push_back(close(u, v, label));
    const auto limit = std::uint64_t(1) << edges.size();
    for (std::uint64_t mask = 0; mask < limit; ++mask) {
      Fixture f(PrimaryComponentEdgeSemantics::Set);
      for (unsigned u = 0; u < n; ++u)
        f.vertex(u);
      for (std::size_t bit = 0; bit < edges.size(); ++bit)
        if ((mask >> bit) & 1U)
          f.insert(edges[bit], false);
      verify(f.solver, f.oracle);
      for (std::size_t bit = 0; bit < edges.size(); ++bit) {
        const bool present = bool((mask >> bit) & 1U);
        if (present) {
          f.erase(edges[bit]);
          f.insert(edges[bit]);
        } else {
          f.insert(edges[bit]);
          f.erase(edges[bit]);
        }
      }
    }
    std::cout << "exhaustive n=" << n << " k=" << k << " graphs=" << limit
              << " directed_transitions=" << limit * edges.size() << '\n';
  }
}

void randomDyck(std::size_t seed_count, std::size_t steps) {
  for (std::size_t seed = 0; seed < seed_count; ++seed) {
    std::mt19937_64 rng(0x514bcb25ULL + seed);
    Fixture f(seed % 2 == 0 ? PrimaryComponentEdgeSemantics::ReferenceCounted
                            : PrimaryComponentEdgeSemantics::Set);
    const std::size_t n = 4 + seed % 7;
    for (std::size_t v = 0; v < n; ++v)
      f.vertex(static_cast<Vertex>(v) * 1000000007 - 31);
    const std::vector<Vertex> ids(f.oracle.vertices.begin(),
                                  f.oracle.vertices.end());
    const std::vector<Label> labels{0, 1, 23,
                                    std::numeric_limits<Label>::max()};
    std::vector<std::string> trace;
    for (std::size_t step = 0; step < steps; ++step) {
      Edge edge =
          close(ids[rng() % n], ids[rng() % n], labels[rng() % labels.size()]);
      bool insert = rng() % 100 < 50;
      // Alternate sparse and dense phases, and delete existing edges often.
      const std::size_t threshold = (step / 100) % 3 == 0 ? n * n : 2 * n;
      if (f.oracle.counts.size() > threshold)
        insert = rng() % 100 < 20;
      if (!f.oracle.counts.empty() &&
          ((!insert && rng() % 4 != 0) || rng() % 7 == 0)) {
        auto it = f.oracle.counts.begin();
        std::advance(
            it, static_cast<std::ptrdiff_t>(rng() % f.oracle.counts.size()));
        edge = close(std::get<0>(it->first), std::get<1>(it->first),
                     std::get<2>(it->first));
      }
      if (rng() % 2)
        edge = open(edge.target, edge.source, edge.label);
      std::ostringstream operation;
      operation << (insert ? "A " : "D ") << edge.source << ' ' << edge.target
                << ' ' << (edge.kind == Parenthesis::Open ? "op--" : "cp--")
                << edge.label;
      trace.push_back(operation.str());
      try {
        const auto before = f.solver.diagnostics();
        if (insert)
          f.insert(edge, false);
        else
          f.erase(edge, false);
        verify(f.solver, f.oracle, step % 37 == 0 || step + 1 == steps);
        const auto after = f.solver.diagnostics();
        CHECK(after.primal_insertions + after.primal_deletions -
                  before.primal_insertions - before.primal_deletions <=
              (insert ? 1U : 3U));
        CHECK(after.affected_vertices - before.affected_vertices <= n);
        CHECK(after.sampled_out_targets - before.sampled_out_targets <=
              2 * n * labels.size());
        CHECK(after.in_primary_visits - before.in_primary_visits <=
              2 * n * labels.size());
        CHECK(after.rebuilt_summary_entries - before.rebuilt_summary_entries <=
              2 * n * labels.size());
        // Each consumed fixpoint list has at least two entries and becomes one:
        // the total number of scanned targets is <= twice the initial list
        // mass.
        CHECK(after.fixpoint_targets - before.fixpoint_targets <=
              6 * n * labels.size() + 2);
        CHECK(after.dscc_cache_vertices - before.dscc_cache_vertices <= n);
        if (test_backend ==
            PrimaryComponentConnectivityBackend::Deterministic) {
          const auto supports =
              after.primal_insertions + after.primal_deletions -
              before.primal_insertions - before.primal_deletions;
          CHECK(after.certificate_edges_scanned -
                    before.certificate_edges_scanned <=
                8 * after.certificate_universe * supports);
          CHECK(after.primal_support_leaves_visited -
                    before.primal_support_leaves_visited <=
                supports);
        }
      } catch (const std::exception &) {
        std::cerr << "Replay: seed=" << seed << " step=" << step << '\n';
        for (const auto &operation_text : trace)
          std::cerr << operation_text << '\n';
        throw;
      }
    }
  }
  std::cout << "random_dyck seeds=" << seed_count << " steps_per_seed=" << steps
            << '\n';
}

using PrimaryCounts =
    std::map<std::pair<std::size_t, std::size_t>, std::uint64_t>;
void verifyPrimary(Primary &primary, std::size_t n, const PrimaryCounts &counts,
                   bool structure) {
  std::vector<std::vector<std::size_t>> adjacency(n);
  for (const auto &entry : counts) {
    adjacency[entry.first.first].push_back(entry.first.second);
    adjacency[entry.first.second].push_back(entry.first.first);
  }
  std::vector<std::size_t> labels(n, n);
  for (std::size_t v = 0; v < n; ++v) {
    if (labels[v] != n)
      continue;
    std::vector<std::size_t> stack{v};
    labels[v] = v;
    while (!stack.empty()) {
      const auto u = stack.back();
      stack.pop_back();
      for (auto w : adjacency[u])
        if (labels[w] == n) {
          labels[w] = v;
          stack.push_back(w);
        }
    }
  }
  for (std::size_t a = 0; a < n; ++a) {
    CHECK(primary.representative(a) < n);
    CHECK(labels[primary.representative(a)] == labels[a]);
    for (std::size_t b = 0; b < n; ++b)
      CHECK(primary.connected(a, b) == (labels[a] == labels[b]));
  }
  PrimaryCounts actual;
  for (const auto &support : primary.supports())
    actual[{support.first, support.second}] = support.count;
  CHECK(actual == counts);
  if (structure) {
    std::string error;
    if (!primary.validate(&error))
      throw std::runtime_error("primary structural invariant: " + error);
  }
}

void connectivity(std::size_t seed_count, std::size_t steps) {
  { // Every simple graph on four nodes, every one-edge transition.
    const std::vector<std::pair<std::size_t, std::size_t>> edges{
        {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
    for (unsigned mask = 0; mask < 64; ++mask) {
      Primary p;
      PrimaryCounts counts;
      for (int i = 0; i < 4; ++i)
        p.addVertex();
      for (std::size_t b = 0; b < edges.size(); ++b)
        if ((mask >> b) & 1U) {
          p.insertEdge(edges[b].first, edges[b].second);
          ++counts[edges[b]];
        }
      verifyPrimary(p, 4, counts, true);
      for (const auto &e : edges) {
        if (counts.count(e)) {
          CHECK(p.deleteEdge(e.first, e.second));
          counts.erase(e);
          verifyPrimary(p, 4, counts, true);
          p.insertEdge(e.first, e.second);
          counts[e] = 1;
        } else {
          p.insertEdge(e.first, e.second);
          counts[e] = 1;
          verifyPrimary(p, 4, counts, true);
          CHECK(p.deleteEdge(e.first, e.second));
          counts.erase(e);
        }
        verifyPrimary(p, 4, counts, true);
      }
    }
  }
  std::uint64_t promotions = 0, replacements = 0;
  std::size_t max_levels = 0;
  for (std::size_t seed = 0; seed < seed_count; ++seed) {
    std::mt19937_64 rng(0x19c33fULL + seed);
    Primary p;
    std::size_t n = 8 + seed % 17;
    for (std::size_t v = 0; v < n; ++v)
      CHECK(p.addVertex() == v);
    PrimaryCounts counts;
    for (std::size_t step = 0; step < steps; ++step) {
      if (step != 0 && step % 251 == 0) {
        CHECK(p.addVertex() == n);
        ++n; // Exercise dynamic vertex growth after HDT has acquired levels.
      }
      auto a = rng() % n, b = rng() % n;
      if (a > b)
        std::swap(a, b);
      auto edge = std::make_pair(a, b);
      const bool insert = rng() % 100 < (counts.size() > 2 * n ? 35U : 60U);
      if (!insert && !counts.empty() && rng() % 4 != 0) {
        auto found = counts.begin();
        std::advance(found, static_cast<std::ptrdiff_t>(rng() % counts.size()));
        edge = found->first;
      }
      if (insert) {
        p.insertEdge(edge.first, edge.second);
        ++counts[edge];
      } else {
        auto found = counts.find(edge);
        CHECK(p.deleteEdge(edge.first, edge.second) == (found != counts.end()));
        if (found != counts.end() && --found->second == 0)
          counts.erase(found);
      }
      verifyPrimary(p, n, counts, step % 31 == 0 || step + 1 == steps);
    }
    const auto stats = p.statistics();
    promotions += stats.promotions;
    replacements += stats.replacements;
    max_levels = std::max(max_levels, stats.levels);
    while (!counts.empty()) {
      const auto e = counts.begin()->first;
      CHECK(p.deleteEdge(e.first, e.second));
      if (--counts.begin()->second == 0)
        counts.erase(counts.begin());
    }
    verifyPrimary(p, n, counts, true);
    CHECK(p.statistics().components == n);
    Primary moved(std::move(p));
    verifyPrimary(moved, n, counts, true);
    p = std::move(moved);
    verifyPrimary(p, n, counts, true);
    throws<std::out_of_range>([&] { p.insertEdge(n, 0); });
    throws<std::out_of_range>([&] { p.deleteEdge(0, n); });
    throws<std::out_of_range>([&] { p.representative(n); });
  }
  CHECK(promotions > 0);
  CHECK(replacements > 0);
  CHECK(max_levels >= 3);
  std::cout << "connectivity seeds=" << seed_count
            << " steps_per_seed=" << steps << " promotions=" << promotions
            << " replacements=" << replacements
            << " maximum_levels=" << max_levels << '\n';
}

void dense() {
  constexpr std::size_t n = 96, k = 2;
  PrimaryComponentSolver solver(PrimaryComponentEdgeSemantics::Set,
                                test_backend);
  std::mt19937_64 rng(8823);
  std::vector<Vertex> targets(n);
  std::iota(targets.begin(), targets.end(), 0);
  for (std::size_t source = 0; source < n; ++source) {
    for (Label label = 0; label < k; ++label) {
      std::shuffle(targets.begin(), targets.end(), rng);
      for (Vertex target : targets)
        solver.insertEdge(close(static_cast<Vertex>(source), target, label));
    }
  }
  CHECK(solver.statistics().edges == n * n * k);
  CHECK(solver.statistics().components == 1);
  // Different permutations yield a genuinely dense sparse-chain union, not
  // merely n copies of the same n-1 primary edges.
  CHECK(solver.diagnostics().primal_edges > n * n / 3);
  std::uint64_t maximum_summaries = 0, maximum_samples = 0;
  for (std::size_t update = 0; update < 200; ++update) {
    const Edge edge =
        close(static_cast<Vertex>(rng() % n), static_cast<Vertex>(rng() % n),
              static_cast<Label>(rng() % k));
    const auto before = solver.diagnostics();
    CHECK(solver.deleteEdge(edge));
    const auto after = solver.diagnostics();
    const auto summaries =
        after.rebuilt_summary_entries - before.rebuilt_summary_entries;
    const auto samples = after.sampled_out_targets - before.sampled_out_targets;
    maximum_summaries = std::max(maximum_summaries, summaries);
    maximum_samples = std::max(maximum_samples, samples);
    CHECK(summaries <= 2 * n * k);
    CHECK(samples <= 2 * n * k);
    CHECK(after.in_primary_visits - before.in_primary_visits <= 2 * n * k);
    CHECK(after.primal_insertions + after.primal_deletions -
              before.primal_insertions - before.primal_deletions <=
          3);
    CHECK(solver.statistics().components == 1);
    CHECK(solver.diagnostics().primary_components == 1);
    CHECK(solver.insertEdge(edge));
  }
  std::string error;
  CHECK(solver.validate(&error));
  std::cout << "dense vertices=" << n << " labels=" << k
            << " edges=" << n * n * k
            << " primal_edges=" << solver.diagnostics().primal_edges
            << " delete_reinsert_pairs=200 max_rebuilt_summaries="
            << maximum_summaries << " max_sampled_targets=" << maximum_samples
            << '\n';
}

void scaling() {
  { // A single update must propagate through thousands of dependent DSCCs.
    constexpr Vertex depth = 2048;
    PrimaryComponentSolver solver(
        PrimaryComponentEdgeSemantics::ReferenceCounted, test_backend);
    solver.insertEdge(close(0, 1));
    solver.insertEdge(close(0, 2));
    for (Vertex i = 0; i < depth; ++i) {
      solver.insertEdge(close(2 * i + 1, 2 * i + 3));
      solver.insertEdge(close(2 * i + 2, 2 * i + 4));
    }
    CHECK(solver.statistics().components ==
          static_cast<std::size_t>(depth + 2));
    for (Vertex i = 0; i <= depth; ++i)
      CHECK(solver.connected(2 * i + 1, 2 * i + 2));
    const auto before = solver.diagnostics();
    CHECK(solver.deleteEdge(close(0, 2)));
    const auto after = solver.diagnostics();
    CHECK(after.affected_vertices - before.affected_vertices ==
          2 * (depth + 1));
    CHECK(solver.statistics().components == solver.statistics().vertices);
    for (Vertex i = 0; i <= depth; ++i)
      CHECK(!solver.connected(2 * i + 1, 2 * i + 2));
    CHECK(solver.insertEdge(close(0, 2)));
    CHECK(solver.statistics().components ==
          static_cast<std::size_t>(depth + 2));
    std::string error;
    CHECK(solver.validate(&error));
    std::cout << "cascade dependency_depth=" << depth << " affected_vertices="
              << after.affected_vertices - before.affected_vertices << '\n';
  }
  { // Balanced recursive cuts force genuinely deep HDT level hierarchies.
    constexpr std::size_t n = 2048;
    Primary primary;
    PrimaryCounts counts;
    for (std::size_t i = 0; i < n; ++i)
      primary.addVertex();
    for (std::size_t i = 1; i < n; ++i) {
      primary.insertEdge(i - 1, i);
      counts[{i - 1, i}] = 1;
    }
    std::vector<std::pair<std::size_t, std::size_t>> intervals{{0, n}};
    std::size_t deletions = 0;
    while (!intervals.empty()) {
      const auto range = intervals.back();
      intervals.pop_back();
      if (range.second - range.first < 2)
        continue;
      const auto midpoint = (range.first + range.second) / 2;
      CHECK(primary.deleteEdge(midpoint - 1, midpoint));
      counts.erase({midpoint - 1, midpoint});
      CHECK(!primary.connected(midpoint - 1, midpoint));
      intervals.push_back({range.first, midpoint});
      intervals.push_back({midpoint, range.second});
      if (++deletions % 127 == 0) {
        std::string error;
        CHECK(primary.validate(&error));
      }
    }
    verifyPrimary(primary, n, counts, true);
    CHECK(primary.statistics().components == n);
    CHECK(primary.statistics().levels >= 10);
    std::cout << "HDT_balanced_cuts vertices=" << n << " cuts=" << deletions
              << " levels=" << primary.statistics().levels
              << " promotions=" << primary.statistics().promotions << '\n';
  }
}

void locality() {
  PrimaryComponentSolver solver(PrimaryComponentEdgeSemantics::ReferenceCounted,
                                test_backend);
  constexpr Vertex n = 20000;
  for (Vertex v = 0; v < n; ++v)
    solver.addVertex(v);
  // Many unrelated components with the same label must remain unvisited.
  for (Vertex u = 3; u + 2 < n; u += 3) {
    solver.insertEdge(close(u, u + 1));
    solver.insertEdge(close(u, u + 2));
  }
  solver.insertEdge(close(0, 1));
  solver.insertEdge(close(0, 2));
  auto before = solver.diagnostics();
  CHECK(solver.deleteEdge(close(0, 2)));
  auto after = solver.diagnostics();
  CHECK(after.affected_vertices - before.affected_vertices == 2);
  CHECK(after.affected_components - before.affected_components == 1);
  CHECK(after.rebuilt_summary_entries - before.rebuilt_summary_entries == 1);
  CHECK(!solver.connected(1, 2));
  CHECK(solver.connected(4, 5));
  // Label churn should reclaim empty OutEdges/InPrimary/Edges label entries.
  for (Label label = 100; label < 1100; ++label) {
    CHECK(solver.insertEdge(close(0, 1, label)));
    CHECK(solver.deleteEdge(close(0, 1, label)));
  }
  std::string error;
  CHECK(solver.validate(&error));
  CHECK(solver.statistics().vertices == static_cast<std::size_t>(n));
  std::cout << "locality total_vertices=" << n
            << " affected_vertices=2 rebuilt_summaries=1\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    const std::string suite = argc >= 2 ? argv[1] : "all";
    const std::size_t seeds = argc >= 3 ? std::stoull(argv[2]) : 24;
    const std::size_t steps = argc >= 4 ? std::stoull(argv[3]) : 1200;
    if (argc >= 5) {
      const std::string backend = argv[4];
      if (backend == "hdt")
        test_backend = PrimaryComponentConnectivityBackend::HDT;
      else if (backend != "deterministic")
        throw std::invalid_argument("unknown backend");
    }
    if (argc > 5)
      throw std::invalid_argument("usage: dynamic_dyck_primary_component_test "
                                  "SUITE [SEEDS [STEPS [deterministic|hdt]]]");
    std::cout << "backend="
              << (test_backend == PrimaryComponentConnectivityBackend::HDT
                      ? "hdt"
                      : "deterministic")
              << '\n';
    const auto start = std::chrono::steady_clock::now();
    bool selected = false;
    auto run = [&](const char *name, auto function) {
      if (suite == "all" || suite == name) {
        selected = true;
        function();
        std::cout << "PASS " << name << '\n';
      }
    };
    run("regressions", regressions);
    run("api", api);
    run("exhaustive", exhaustive);
    run("random", [&] { randomDyck(seeds, steps); });
    run("connectivity", [&] { connectivity(seeds, steps); });
    run("dense", dense);
    run("locality", locality);
    run("scaling", scaling);
    if (!selected)
      throw std::invalid_argument("unknown test suite: " + suite);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::cout << "checks=" << checks << " semantic_states=" << semantic_states
              << " checked_ordered_pairs=" << checked_pairs
              << " seconds=" << seconds << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
