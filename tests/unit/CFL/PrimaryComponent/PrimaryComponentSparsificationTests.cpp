// SPDX-License-Identifier: MIT
#include "CFL/DynamicDyck/PrimaryComponent/SparsifiedConnectivity.h"
#include "TestSupport.h"

#include <iostream>
#include <numeric>
#include <random>

namespace {
using namespace test;
using Primary =
    lotus::cfl::dynamic_dyck::primary_component::SparsifiedConnectivity;
using Node = Primary::Node;
using Pair = std::pair<Node, Node>;
using Counts = std::map<Pair, std::uint64_t>;
std::uint64_t updates = 0, pair_checks = 0, max_scanned = 0;

void budget(const Primary::Statistics &before,
            const Primary::Statistics &after) {
  const auto N = after.universe;
  const auto scanned =
      after.certificate_edges_scanned - before.certificate_edges_scanned;
  max_scanned = std::max(max_scanned, scanned);
  // At block width b, at most four child forests, each with < b edges;
  // sum(b) < 2N. These are per-operation bounds, NOT sequence averages.
  CHECK(scanned <= 8 * N);
  CHECK(after.certificate_vertices - before.certificate_vertices <= 4 * N);
  CHECK(after.representative_writes - before.representative_writes <=
        2 * after.vertices);
  CHECK(after.certificate_rebuilds - before.certificate_rebuilds <=
        after.levels - 1);
  CHECK(after.path_nodes_visited - before.path_nodes_visited <= after.levels);
  CHECK(after.support_leaves_visited - before.support_leaves_visited <= 1);
}
void update(Primary &p, Counts &counts, Pair e, bool insert) {
  e = {std::min(e.first, e.second), std::max(e.first, e.second)};
  const auto old = counts.find(e);
  const auto multiplicity = old == counts.end() ? 0 : old->second;
  const auto before = p.statistics();
  if (insert) {
    p.insertEdge(e.first, e.second);
    ++counts[e];
  } else {
    CHECK(p.deleteEdge(e.first, e.second) == (multiplicity != 0));
    if (old != counts.end() && --old->second == 0)
      counts.erase(old);
  }
  const auto after = p.statistics();
  budget(before, after);
  if ((insert && multiplicity != 0) || (!insert && multiplicity != 1)) {
    CHECK(after.certificate_rebuilds == before.certificate_rebuilds);
    CHECK(after.representative_writes == before.representative_writes);
  }
  ++updates;
}
void verify(Primary &p, Node n, const Counts &counts, bool structural) {
  std::vector<std::vector<Node>> adj(n);
  for (const auto &e : counts) {
    adj[e.first.first].push_back(e.first.second);
    adj[e.first.second].push_back(e.first.first);
  }
  std::vector<Node> label(n, n);
  std::size_t components = 0;
  for (Node start = 0; start < n; ++start) {
    if (label[start] != n)
      continue;
    ++components;
    std::vector<Node> todo{start};
    label[start] = start;
    while (!todo.empty()) {
      const Node v = todo.back();
      todo.pop_back();
      for (Node w : adj[v])
        if (label[w] == n) {
          label[w] = start;
          todo.push_back(w);
        }
    }
  }
  for (Node u = 0; u < n; ++u) {
    CHECK(p.representative(u) < n);
    CHECK(label[p.representative(u)] == label[u]);
    for (Node v = 0; v < n; ++v) {
      CHECK(p.connected(u, v) == (label[u] == label[v]));
      ++pair_checks;
    }
  }
  Counts actual;
  for (const auto &e : p.supports())
    actual[{e.first, e.second}] = e.count;
  CHECK(actual == counts);
  CHECK(p.statistics().edges == counts.size());
  CHECK(p.statistics().components == components);
  if (structural) {
    std::string error;
    if (!p.validate(&error))
      throw std::runtime_error(error);
  }
}
void exhaustive() {
  std::vector<Pair> edges;
  for (Node u = 0; u < 5; ++u)
    for (Node v = u + 1; v < 5; ++v)
      edges.emplace_back(u, v);
  for (unsigned mask = 0; mask < 1024; ++mask) {
    Primary p;
    Counts counts;
    for (Node v = 0; v < 5; ++v)
      CHECK(p.addVertex() == v);
    for (std::size_t b = 0; b < edges.size(); ++b)
      if ((mask >> b) & 1U)
        update(p, counts, edges[b], true);
    verify(p, 5, counts, true);
    for (std::size_t b = 0; b < edges.size(); ++b) {
      const bool present = bool((mask >> b) & 1U);
      update(p, counts, edges[b], !present);
      verify(p, 5, counts, true);
      update(p, counts, edges[b], present);
      verify(p, 5, counts, true);
    }
  }
  std::cout << "exhaustive primary_graph_states=1024 "
               "directed_transitions=10240 plus_inverses\n";
}
void random(std::size_t seeds, std::size_t steps) {
  for (std::size_t seed = 0; seed < seeds; ++seed) {
    Primary p;
    Counts counts;
    Node n = 1 + seed % 23;
    for (Node v = 0; v < n; ++v)
      p.addVertex();
    std::mt19937_64 rng(682380 + seed);
    for (std::size_t i = 0; i < steps; ++i) {
      if (i % 97 == 96) {
        CHECK(p.addVertex() == n);
        ++n;
      }
      Pair e{rng() % n, rng() % n};
      const bool insert = rng() % 100 < (counts.size() > 2 * n ? 35U : 60U);
      if (!insert && !counts.empty() && rng() % 4) {
        auto found = counts.begin();
        std::advance(found, static_cast<std::ptrdiff_t>(rng() % counts.size()));
        e = found->first;
      }
      update(p, counts, e, insert);
      verify(p, n, counts, i % 31 == 0);
    }
    while (!counts.empty())
      update(p, counts, counts.begin()->first, false);
    verify(p, n, counts, true);
    CHECK(p.statistics().blocks == 0); // No dead sparsification paths retained.
    Primary moved(std::move(p));
    throws<std::logic_error>([&] { p.representative(0); });
    p = std::move(moved);
    verify(p, n, counts, true);
    throws<std::out_of_range>([&] { p.insertEdge(n, 0); });
    throws<std::out_of_range>([&] { p.deleteEdge(0, n); });
    throws<std::out_of_range>([&] { p.representative(n); });
  }
  std::cout << "random_primary seeds=" << seeds << " steps_per_seed=" << steps
            << '\n';
}
void adversarial() {
  { // Two dense halves separated by one bridge. Original edges are quadratic;
    // even a true component split may inspect only one original support leaf.
    constexpr Node n = 256;
    Primary p;
    Counts counts;
    for (Node v = 0; v < n; ++v)
      p.addVertex();
    for (Node u = 0; u < n; ++u)
      for (Node v = u + 1; v < n; ++v)
        if ((u < n / 2) == (v < n / 2))
          update(p, counts, {u, v}, true);
    const auto dense_edges = counts.size();
    for (Node i = 0; i < 48; ++i) {
      const Pair bridge{i, n / 2 + i};
      update(p, counts, bridge, true);
      CHECK(p.statistics().components == 1);
      update(p, counts, bridge, false);
      CHECK(p.statistics().components == 2);
      if (i % 11 == 0)
        verify(p, n, counts, true);
    }
    std::cout << "dense_cut vertices=" << n << " original_edges=" << dense_edges
              << " bridge_rounds=48 max_original_leaves_per_update=1\n";
  }
  { // Grow the universe repeatedly while old, nontrivial certificates survive.
    Primary p;
    Counts counts;
    for (Node v = 0; v < 17; ++v)
      p.addVertex();
    for (Node v = 1; v < 17; ++v)
      update(p, counts, {v - 1, v}, true);
    update(p, counts, {0, 16}, true);
    for (Node v = 17; v < 8193; ++v)
      CHECK(p.addVertex() == v);
    update(p, counts, {0, 8192}, true);
    CHECK(p.connected(16, 8192));
    CHECK(!p.connected(4096, 8192));
    update(p, counts, {0, 8192}, false);
    CHECK(!p.connected(16, 8192));
    CHECK(p.connected(0, 16));
    std::string error;
    CHECK(p.validate(&error));
    CHECK(p.statistics().universe == 16384);
    CHECK(p.statistics().levels == 15);
    std::cout << "universe_growth vertices=8193 levels=15\n";
  }
  { // Empty graph, single-vertex root leaf, support reversal and multiplicity.
    Primary p;
    Counts counts;
    std::string error;
    CHECK(p.validate(&error));
    p.addVertex();
    for (bool insert : {true, true, false, false, false}) {
      update(p, counts, {0, 0}, insert);
      verify(p, 1, counts, true);
    }
    p.addVertex();
    update(p, counts, {1, 0}, true);
    update(p, counts, {0, 1}, true);
    update(p, counts, {1, 0}, false);
    CHECK(p.connected(0, 1));
    update(p, counts, {0, 1}, false);
    CHECK(!p.connected(0, 1));
    verify(p, 2, counts, true);
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    const std::size_t seeds = argc > 1 ? std::stoull(argv[1]) : 32;
    const std::size_t steps = argc > 2 ? std::stoull(argv[2]) : 1200;
    if (argc > 3 || seeds == 0 || steps == 0)
      throw std::invalid_argument(
          "usage: dynamic_dyck_primary_component_sparsification [SEEDS "
          "[STEPS]]");
    exhaustive();
    random(seeds, steps);
    adversarial();
    std::cout << "PASS sparsification updates=" << updates
              << " pair_checks=" << pair_checks << " checks=" << checks
              << " max_certificate_input_edges=" << max_scanned << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL sparsification: " << e.what() << '\n';
    return 1;
  }
}
