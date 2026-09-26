#include "CFL/DynamicDyck/WeightedQuotient/WeightedQuotientSolver.h"

#include <algorithm>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::dynamic_dyck {
namespace {

using EdgeKey = std::tuple<Vertex, Vertex, Label>;

EdgeKey key(Edge edge) {
  edge = edge.opening();
  return {edge.source, edge.target, edge.label};
}

bool updateGraph(Graph &graph, Update update) {
  const auto existing =
      std::find_if(graph.edges.begin(), graph.edges.end(),
                   [&](Edge edge) { return key(edge) == key(update.edge); });
  if (update.kind == UpdateKind::Insert) {
    if (existing != graph.edges.end())
      return false;
    graph.edges.push_back(update.edge.opening());
    return true;
  }
  if (existing == graph.edges.end())
    return false;
  graph.edges.erase(existing);
  return true;
}

// Independent boolean CFL closure for S -> epsilon | SS | op_i S cp_i.
// No union-find, quotient, or production solver code is used in this oracle.
std::vector<std::vector<bool>> oracle(const Graph &graph) {
  const std::size_t count = graph.vertices.size();
  auto index = [&](Vertex vertex) {
    return static_cast<std::size_t>(
        std::find(graph.vertices.begin(), graph.vertices.end(), vertex) -
        graph.vertices.begin());
  };
  std::vector<std::vector<bool>> relation(count,
                                          std::vector<bool>(count, false));
  for (std::size_t node = 0; node < count; ++node)
    relation[node][node] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    auto derive = [&](std::size_t source, std::size_t target) {
      if (!relation[source][target]) {
        relation[source][target] = true;
        changed = true;
      }
    };
    for (std::size_t middle = 0; middle < count; ++middle)
      for (std::size_t source = 0; source < count; ++source)
        for (std::size_t target = 0; target < count; ++target)
          if (relation[source][middle] && relation[middle][target])
            derive(source, target);
    for (auto first : graph.edges) {
      first = first.opening();
      for (auto second : graph.edges) {
        second = second.opening();
        if (first.label == second.label &&
            relation[index(first.target)][index(second.target)])
          derive(index(first.source), index(second.source));
      }
    }
  }
  return relation;
}

void expectOracle(const WeightedQuotientSolver &solver, const Graph &graph) {
  const auto expected = oracle(graph);
  for (std::size_t source = 0; source < graph.vertices.size(); ++source)
    for (std::size_t target = 0; target < graph.vertices.size(); ++target)
      ASSERT_EQ(
          solver.connected(graph.vertices[source], graph.vertices[target]),
          expected[source][target])
          << "source=" << graph.vertices[source]
          << " target=" << graph.vertices[target];
  EXPECT_EQ(solver.statistics().edges, graph.edges.size());
  EXPECT_EQ(solver.statistics().components, solver.components().size());
  std::vector<Vertex> reported;
  for (const auto &component : solver.components())
    reported.insert(reported.end(), component.begin(), component.end());
  std::sort(reported.begin(), reported.end());
  auto vertices = graph.vertices;
  std::sort(vertices.begin(), vertices.end());
  EXPECT_EQ(reported, vertices);
}

TEST(DynamicDyckTest, SupportsSparseVerticesAndIndependentInstances) {
  WeightedQuotientSolver first, second;
  EXPECT_FALSE(first.connected(0, 0));
  EXPECT_TRUE(first.addVertex(-100));
  EXPECT_FALSE(first.addVertex(-100));
  EXPECT_TRUE(first.connected(-100, -100));
  EXPECT_THROW(first.representative(0), std::out_of_range);
  const Vertex large = std::numeric_limits<Vertex>::max();
  first.insertEdge({-100, large, 17});
  first.insertEdge({23, large, 17});
  EXPECT_TRUE(first.connected(-100, 23));
  EXPECT_FALSE(first.connected(-100, large));
  second.insertEdge({-100, large, 18});
  second.insertEdge({23, large, 17});
  EXPECT_FALSE(second.connected(-100, 23));
  EXPECT_EQ(first.representative(-100), first.representative(23));
  EXPECT_EQ(first.statistics().vertices, 3U);
  const Graph exported = first.graph();
  WeightedQuotientSolver reconstructed(exported);
  EXPECT_EQ(first.components(), reconstructed.components());
  WeightedQuotientSolver moved(std::move(first));
  EXPECT_TRUE(moved.connected(-100, 23));
  reconstructed = std::move(moved);
  EXPECT_TRUE(reconstructed.deleteEdge({23, large, 17}));
  EXPECT_FALSE(reconstructed.connected(-100, 23));
  EXPECT_FALSE(second.connected(-100, 23));
}

TEST(DynamicDyckTest, TreatsComplementaryDuplicatesAndMissingDeletionsAsNoOps) {
  WeightedQuotientSolver solver;
  EXPECT_TRUE(solver.insertEdge({0, 1, 0}));
  EXPECT_FALSE(solver.insertEdge({0, 1, 0}));
  EXPECT_FALSE(solver.insertEdge({1, 0, 0, Parenthesis::Close}));
  EXPECT_FALSE(solver.deleteEdge({9, 10, 0}));
  EXPECT_EQ(solver.statistics().vertices, 2U);
  EXPECT_EQ(solver.statistics().edges, 1U);
  EXPECT_TRUE(solver.deleteEdge({1, 0, 0, Parenthesis::Close}));
  EXPECT_FALSE(solver.deleteEdge({0, 1, 0}));
  EXPECT_EQ(solver.statistics().edges, 0U);
  EXPECT_EQ(solver.statistics().insertions, 1U);
  EXPECT_EQ(solver.statistics().deletions, 1U);
  EXPECT_THROW(solver.insertEdge({4, 5, 0, static_cast<Parenthesis>(100)}),
               std::invalid_argument);
  EXPECT_THROW(solver.apply({static_cast<UpdateKind>(100), {}}),
               std::invalid_argument);
  EXPECT_EQ(solver.statistics().vertices, 2U);
}

TEST(DynamicDyckTest, PropagatesAcyclicSplitsAndPreservesOtherWitnesses) {
  Graph graph{{0, 1, 2, 3, 4, 5},
              {{0, 2, 0}, {1, 2, 0}, {3, 0, 1}, {4, 1, 1}, {5, 2, 0}}};
  WeightedQuotientSolver solver(graph);
  EXPECT_TRUE(solver.connected(3, 4));
  EXPECT_TRUE(solver.deleteEdge({1, 2, 0}));
  updateGraph(graph, {UpdateKind::Delete, {1, 2, 0}});
  EXPECT_FALSE(solver.connected(0, 1));
  EXPECT_FALSE(solver.connected(3, 4));
  EXPECT_TRUE(solver.connected(0, 5));
  EXPECT_EQ(solver.statistics().cycle_rebuilds, 0U);
  expectOracle(solver, graph);
  EXPECT_TRUE(solver.insertEdge({1, 2, 0}));
  updateGraph(graph, {UpdateKind::Insert, {1, 2, 0}});
  EXPECT_TRUE(solver.connected(3, 4));
  expectOracle(solver, graph);
}

TEST(DynamicDyckTest, BreaksCyclicSupportAfterAnchorDeletion) {
  // u,v merge at w; x,y merge through u,v and then spuriously support u,v.
  // Removing v->w must undo both merges (Zhang's 2024 cycle counterexample).
  Graph graph{{0, 1, 2, 3, 4, 10, 11, 12},
              {{0, 2, 0},
               {1, 2, 0},
               {3, 0, 0},
               {4, 1, 0},
               {0, 3, 1},
               {1, 4, 1},
               {10, 12, 2},
               {11, 12, 2}}};
  WeightedQuotientSolver solver(graph);
  EXPECT_TRUE(solver.connected(0, 1));
  EXPECT_TRUE(solver.connected(3, 4));
  EXPECT_TRUE(solver.deleteEdge({1, 2, 0}));
  updateGraph(graph, {UpdateKind::Delete, {1, 2, 0}});
  EXPECT_FALSE(solver.connected(0, 1));
  EXPECT_FALSE(solver.connected(3, 4));
  EXPECT_TRUE(solver.connected(10, 11));
  EXPECT_EQ(solver.statistics().cycle_rebuilds, 1U);
  expectOracle(solver, graph);
}

TEST(DynamicDyckTest, PropagatesDeletionThroughLongAcyclicChains) {
  constexpr Vertex depth = 2048;
  Graph graph;
  for (Vertex node = 0; node <= 2 * depth; ++node)
    graph.vertices.push_back(node);
  for (Vertex level = 0; level < depth; ++level) {
    graph.edges.push_back({2 * level, 2 * level + 2, 0});
    graph.edges.push_back(
        {2 * level + 1, level + 1 == depth ? 2 * depth : 2 * level + 3, 0});
  }
  WeightedQuotientSolver solver(graph);
  for (Vertex level = 0; level < depth; ++level)
    ASSERT_TRUE(solver.connected(2 * level, 2 * level + 1));
  ASSERT_TRUE(solver.deleteEdge({2 * depth - 1, 2 * depth, 0}));
  for (Vertex level = 0; level < depth; ++level)
    ASSERT_FALSE(solver.connected(2 * level, 2 * level + 1));
  EXPECT_EQ(solver.statistics().components,
            static_cast<std::size_t>(2 * depth + 1));
  EXPECT_EQ(solver.statistics().cycle_rebuilds, 0U);
}

void exhaustiveUpdates(const std::vector<Edge> &candidates) {
  for (unsigned mask = 0; mask < (1U << candidates.size()); ++mask) {
    SCOPED_TRACE(::testing::Message() << "mask=" << mask);
    Graph graph{{0, 1, 2}, {}};
    for (std::size_t edge = 0; edge < candidates.size(); ++edge)
      if ((mask & (1U << edge)) != 0)
        graph.edges.push_back(candidates[edge]);
    WeightedQuotientSolver solver(graph);
    expectOracle(solver, graph);
    for (std::size_t edge = 0; edge < candidates.size(); ++edge) {
      SCOPED_TRACE(::testing::Message() << "edge=" << edge);
      const bool present = (mask & (1U << edge)) != 0;
      const Update change{present ? UpdateKind::Delete : UpdateKind::Insert,
                          candidates[edge]};
      ASSERT_EQ(solver.apply(change), updateGraph(graph, change));
      expectOracle(solver, graph);
      EXPECT_FALSE(solver.apply(change));
      const Update undo{present ? UpdateKind::Insert : UpdateKind::Delete,
                        candidates[edge]};
      ASSERT_EQ(solver.apply(undo), updateGraph(graph, undo));
      expectOracle(solver, graph);
    }
  }
}

TEST(DynamicDyckTest,
     MatchesCFLOracleAcrossAllThreeNodeSingleLabelGraphsAndUpdates) {
  std::vector<Edge> candidates;
  for (Vertex source = 0; source < 3; ++source)
    for (Vertex target = 0; target < 3; ++target)
      candidates.push_back({source, target, 0});
  exhaustiveUpdates(candidates);
}

TEST(DynamicDyckTest, MatchesCFLOracleAcrossMultilabelGraphsAndUpdates) {
  exhaustiveUpdates({{0, 1, 0},
                     {0, 1, 1},
                     {1, 2, 0},
                     {2, 1, 1},
                     {2, 0, 0},
                     {0, 0, 1},
                     {1, 1, 0},
                     {2, 2, 1}});
}

TEST(DynamicDyckTest, MatchesCFLOracleThroughoutRandomMixedSequences) {
  for (unsigned seed : {7U, 42U, 123U, 9876U}) {
    std::mt19937 random(seed);
    Graph graph{{0, 1, 2, 3, 4, 5, 6}, {}};
    WeightedQuotientSolver solver(graph);
    for (unsigned step = 0; step < 1000; ++step) {
      SCOPED_TRACE(::testing::Message() << "seed=" << seed << " step=" << step);
      Edge edge{static_cast<Vertex>(random() % 7),
                static_cast<Vertex>(random() % 7),
                static_cast<Label>(random() % 4)};
      if (random() % 2 != 0) {
        std::swap(edge.source, edge.target);
        edge.kind = Parenthesis::Close;
      }
      const Update update{
          random() % 3 == 0 ? UpdateKind::Insert : UpdateKind::Delete, edge};
      ASSERT_EQ(solver.apply(update), updateGraph(graph, update));
      expectOracle(solver, graph);
    }
  }
}

TEST(DynamicDyckTest, ReadsArtifactFormatsAndRejectsMalformedInputs) {
  std::istringstream dot("digraph G {\n// comment\n-3;\n"
                         "0 -> 1 [ label = \"op--0\" ];\n"
                         "1->0[label=\"cp--0\"]\n2->1[label=\"op--0\"]\n}\n");
  WeightedQuotientSolver solver(parseDot(dot));
  EXPECT_EQ(solver.statistics().vertices, 4U);
  EXPECT_EQ(solver.statistics().edges, 2U);
  EXPECT_TRUE(solver.connected(0, 2));
  std::istringstream sequence("# comment\nD 1 2 cp--0\nA 2 1 op--0\n");
  const auto updates = parseUpdates(sequence);
  ASSERT_EQ(updates.size(), 2U);
  EXPECT_TRUE(solver.apply(updates[0]));
  EXPECT_FALSE(solver.connected(0, 2));
  EXPECT_TRUE(solver.apply(updates[1]));
  EXPECT_TRUE(solver.connected(0, 2));
  for (const std::string &text :
       {"0->1[label=\"ob--0\"]", "0->1[label=\"normal\"]",
        "0->1[label=\"op--4294967296\"]", "0->1[label=\"op--\"]",
        "0->1[label=\"op--0junk\"]", "0->oops[label=\"op--0\"]",
        "9223372036854775808->0[label=\"op--0\"]", "\"0->1[label=\"op--0\"]"}) {
    std::istringstream input(text);
    EXPECT_THROW(parseDot(input), std::invalid_argument) << text;
  }
  for (const std::string &text :
       {"X 0 1 op--0", "A 0 1 ob--0", "A 0 1 op--0 extra", "D 0 1",
        "A bad 1 op--0", "A 0 1 cp---1"}) {
    std::istringstream input(text);
    EXPECT_THROW(parseUpdates(input), std::invalid_argument) << text;
  }
  std::istringstream empty_dot, empty_sequence;
  EXPECT_TRUE(parseDot(empty_dot).vertices.empty());
  EXPECT_TRUE(parseUpdates(empty_sequence).empty());
}

} // namespace
} // namespace lotus::cfl::dynamic_dyck
