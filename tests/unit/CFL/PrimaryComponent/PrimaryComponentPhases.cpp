// SPDX-License-Identifier: MIT
#include "PrimaryComponentTestAccess.h"
#include "TestSupport.h"

#include <iostream>
#include <limits>
#include <random>

namespace {
using namespace test;
using namespace lotus::cfl::dynamic_dyck::primary_component;
using Node = TestSnapshot::Node;
using Slot = std::pair<Node, Label>;
using Summary = std::tuple<Node, Label, Node>;
std::uint64_t insertion_checkpoints = 0, deletion_checkpoints = 0,
              forward_checkpoints = 0;

struct PhaseFixture {
  PrimaryComponentSolver solver;
  Oracle oracle;
  TestSnapshot before;
  Edge operation{};
  std::map<std::pair<Vertex, Label>, std::vector<Vertex>> lists;
  std::set<Node> affected;
  explicit PhaseFixture(PrimaryComponentConnectivityBackend backend,
                        PrimaryComponentEdgeSemantics semantics)
      : solver(semantics, backend), oracle(semantics) {
    TestAccess::observe(solver, [&](const TestSnapshot &s) { check(s); });
  }
  void vertex(Vertex v) {
    solver.addVertex(v);
    oracle.vertices.insert(v);
  }
  void common(const TestSnapshot &s) {
    const auto n = s.vertices.size();
    CHECK(s.dscc.size() == n && s.primary.size() == n && s.parent.size() == n);
    std::map<std::pair<Vertex, Label>, std::vector<Vertex>> actual_lists;
    std::set<Triple> original;
    std::map<Slot, std::set<Node>> expected_incoming, actual_incoming;
    for (const auto &list : s.out_edges) {
      CHECK(list.source < n && !list.targets.empty());
      auto &out = actual_lists[{s.vertices[list.source], list.label}];
      for (Node t : list.targets) {
        CHECK(t < n);
        CHECK(
            original.emplace(s.vertices[list.source], s.vertices[t], list.label)
                .second);
        out.push_back(s.vertices[t]);
      }
      expected_incoming[{list.targets.back(), list.label}].insert(list.source);
    }
    CHECK(actual_lists ==
          lists); // Exact head/tail order, not just an edge set.
    std::set<Triple> expected_original;
    for (const auto &entry : oracle.counts)
      expected_original.insert(entry.first);
    CHECK(original == expected_original);
    for (const auto &list : s.in_primary) {
      CHECK(list.source < n && !list.targets.empty());
      for (Node u : list.targets) {
        CHECK(u < n);
        CHECK((actual_incoming[{list.source, list.label}].insert(u).second));
      }
    }
    CHECK(actual_incoming ==
          expected_incoming); // Lemmas 3.2 and 3.3, exact tails.
    const auto relation = oracle.relations();
    std::map<Vertex, Node> rank;
    for (Node i = 0; i < n; ++i)
      rank[relation.vertices[i]] = i;
    for (Node i = 0; i < n; ++i) {
      CHECK(s.dscc[i] < n && s.primary[i] < n);
      CHECK(s.dscc[s.dscc[i]] == s.dscc[i]);
      CHECK(s.primary[s.primary[i]] == s.primary[i]);
      for (Node j = 0; j < n; ++j)
        CHECK((s.primary[i] == s.primary[j]) ==
              bool(relation.primary[rank.at(s.vertices[i])]
                                   [rank.at(s.vertices[j])]));
    }
  }
  void prepared(const TestSnapshot &s) {
    const auto relation = oracle.relations();
    std::map<Vertex, Node> id, rank;
    for (Node i = 0; i < s.vertices.size(); ++i) {
      id[s.vertices[i]] = i;
      rank[relation.vertices[i]] = i;
    }
    // Lemma 3.4(1): before Fixpoint, every current component must already be
    // sound in the NEW graph, not merely in the pre-deletion graph.
    for (Node i = 0; i < s.vertices.size(); ++i)
      for (Node j = 0; j < s.vertices.size(); ++j)
        if (s.dscc[i] == s.dscc[j])
          CHECK(relation.dyck[rank.at(s.vertices[i])][rank.at(s.vertices[j])]);
    std::set<Summary> actual, expected;
    std::set<Slot> needs_queue, queued;
    for (const auto &entry : oracle.counts) {
      const auto &e = entry.first;
      expected.emplace(s.dscc[id.at(std::get<0>(e))], std::get<2>(e),
                       s.dscc[id.at(std::get<1>(e))]);
    }
    for (const auto &list : s.summaries) {
      CHECK(s.dscc[list.source] == list.source && !list.targets.empty());
      for (Node target : list.targets)
        actual.emplace(list.source, list.label, s.dscc.at(target));
      if (list.targets.size() >= 2)
        needs_queue.emplace(list.source, list.label);
    }
    CHECK(actual ==
          expected); // BOTH directions of Lemma 3.4(2), not just saturation.
    for (const auto &item : s.queue)
      CHECK(queued.insert(item).second);
    CHECK(queued ==
          needs_queue); // Includes documented outgoing-summary normalization.
  }
  void check(const TestSnapshot &s) {
    common(s);
    if (s.phase == TestPhase::AffectedDiscovered) {
      ++forward_checkpoints;
      CHECK(s.dscc == before.dscc);
      std::map<Vertex, Node> id;
      for (Node i = 0; i < s.vertices.size(); ++i)
        id[s.vertices[i]] = i;
      // Literal Algorithm 4 line 7: enumerate ALL original edges in the test,
      // and collect DISTINCT TARGET NODES, unlike production's two-node sample.
      std::map<Slot, std::set<Node>> targets;
      for (const auto &entry : oracle.counts) {
        const auto &e = entry.first;
        targets[{s.dscc[id.at(std::get<0>(e))], std::get<2>(e)}].insert(
            id.at(std::get<1>(e)));
      }
      const Node start = s.dscc[id.at(std::get<1>(key(operation)))];
      affected = {start};
      std::vector<Node> todo{start};
      while (!todo.empty()) {
        const Node root = todo.back();
        todo.pop_back();
        for (const auto &group : targets) {
          if (group.first.first != root || group.second.size() < 2)
            continue;
          const Node next = s.dscc[*group.second.begin()];
          if (affected.insert(next).second)
            todo.push_back(next);
        }
      }
      CHECK(affected == std::set<Node>(s.affected.begin(), s.affected.end()));
      return;
    }
    prepared(s);
    if (s.phase == TestPhase::DeletionPrepared) {
      ++deletion_checkpoints;
      CHECK(affected == std::set<Node>(s.affected.begin(), s.affected.end()));
      for (Node v = 0; v < s.vertices.size(); ++v) {
        if (affected.count(before.dscc[v]))
          CHECK(s.dscc[v] == s.primary[v]);
        else
          CHECK(s.dscc[v] == before.dscc[v]);
      }
    } else {
      CHECK(s.phase == TestPhase::InsertionPrepared);
      ++insertion_checkpoints;
      for (Node v = 0; v < before.vertices.size(); ++v)
        CHECK(s.dscc[v] == before.dscc[v]);
    }
  }
  void update(Edge e, bool insert) {
    operation = e;
    before = TestAccess::snapshot(solver);
    const auto triple = key(e);
    const auto old = oracle.counts.find(triple);
    const auto count = old == oracle.counts.end() ? 0 : old->second;
    const bool changed = insert ? oracle.insert(e) : oracle.erase(e);
    const auto slot = std::make_pair(std::get<0>(triple), std::get<2>(triple));
    if (insert && count == 0)
      lists[slot].insert(lists[slot].begin(), std::get<1>(triple));
    if (!insert && count == 1) {
      auto &list = lists.at(slot);
      list.erase(std::find(list.begin(), list.end(), std::get<1>(triple)));
      if (list.empty())
        lists.erase(slot);
    }
    CHECK((insert ? solver.insertEdge(e) : solver.deleteEdge(e)) == changed);
    const auto boundary = TestAccess::snapshot(solver);
    if (solver.connectivityBackend() ==
        PrimaryComponentConnectivityBackend::Deterministic)
      CHECK(boundary.parent ==
            boundary.dscc); // Observe BEFORE any validating finds.
    verify(solver, oracle);
  }
};

void exhaustive(PrimaryComponentConnectivityBackend backend) {
  // All 512 states, every edge transition, and its inverse; phase contracts
  // checked before saturation, so a lucky final fixpoint cannot mask a
  // violation.
  std::vector<Edge> edges;
  for (Vertex u = 0; u < 3; ++u)
    for (Vertex v = 0; v < 3; ++v)
      edges.push_back(close(u, v));
  for (unsigned mask = 0; mask < 512; ++mask) {
    PhaseFixture f(backend, PrimaryComponentEdgeSemantics::Set);
    for (Vertex v = 0; v < 3; ++v)
      f.vertex(v);
    for (std::size_t i = 0; i < edges.size(); ++i)
      if ((mask >> i) & 1U)
        f.update(edges[i], true);
    for (std::size_t i = 0; i < edges.size(); ++i) {
      const bool present = bool((mask >> i) & 1U);
      f.update(edges[i], !present);
      f.update(edges[i], present);
    }
  }
}
void random(PrimaryComponentConnectivityBackend backend) {
  for (std::uint64_t seed = 0; seed < 24; ++seed) {
    PhaseFixture f(backend,
                   seed % 2 ? PrimaryComponentEdgeSemantics::Set
                            : PrimaryComponentEdgeSemantics::ReferenceCounted);
    const std::vector<Vertex> vertices{
        std::numeric_limits<Vertex>::min(), -111, 0, 9, 414,
        std::numeric_limits<Vertex>::max()};
    const std::vector<Label> labels{0, 17, std::numeric_limits<Label>::max()};
    for (Vertex v : vertices)
      f.vertex(v);
    std::mt19937_64 rng(773811 + seed);
    for (std::size_t step = 0; step < 500; ++step) {
      Edge e = close(vertices[rng() % vertices.size()],
                     vertices[rng() % vertices.size()],
                     labels[rng() % labels.size()]);
      const bool insert = rng() % 2 == 0;
      if (!insert && !f.oracle.counts.empty() && rng() % 4) {
        auto it = f.oracle.counts.begin();
        std::advance(
            it, static_cast<std::ptrdiff_t>(rng() % f.oracle.counts.size()));
        e = close(std::get<0>(it->first), std::get<1>(it->first),
                  std::get<2>(it->first));
      }
      if (rng() % 2) {
        std::swap(e.source, e.target);
        e.kind = Parenthesis::Open;
      }
      try {
        f.update(e, insert);
      } catch (...) {
        std::cerr << "phase seed=" << seed << " step=" << step << '\n';
        throw;
      }
    }
  }
}
} // namespace
int main() {
  try {
    for (auto backend : {PrimaryComponentConnectivityBackend::Deterministic,
                         PrimaryComponentConnectivityBackend::HDT}) {
      exhaustive(backend);
      random(backend);
    }
    std::cout << "PASS phase invariants: insertion_prepared="
              << insertion_checkpoints
              << " forward_search=" << forward_checkpoints
              << " deletion_prepared=" << deletion_checkpoints
              << " checks=" << checks << " semantic_states=" << semantic_states
              << " checked_pairs=" << checked_pairs << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL phases: " << e.what() << '\n';
    return 1;
  }
}
