#include "CFL/DynamicDyck/PrimaryComponent/PrimaryComponentSolver.h"
#include "CFL/DynamicDyck/WeightedQuotient/WeightedQuotientSolver.h"

#include <limits>
#include <random>
#include <sstream>
#include <utility>

#include <gtest/gtest.h>

using namespace lotus::cfl::dynamic_dyck;

namespace {
class PrimaryComponentIntegrationTest
    : public ::testing::TestWithParam<PrimaryComponentConnectivityBackend> {};

TEST_P(PrimaryComponentIntegrationTest,
       MatchesExistingSetSolverThroughUpdates) {
  Graph graph;
  for (Vertex vertex = -3; vertex < 5; ++vertex)
    graph.vertices.push_back(vertex);
  WeightedQuotientSolver original(graph);
  PrimaryComponentSolver added(graph, PrimaryComponentEdgeSemantics::Set,
                               GetParam());
  std::mt19937 random(3632884);
  for (unsigned step = 0; step < 600; ++step) {
    const Edge edge{static_cast<Vertex>(random() % 8) - 3,
                    static_cast<Vertex>(random() % 8) - 3,
                    static_cast<Label>(random() % 3),
                    random() % 2 ? Parenthesis::Open : Parenthesis::Close};
    const bool insertion = random() % 2;
    SCOPED_TRACE(step);
    EXPECT_EQ(insertion ? original.insertEdge(edge) : original.deleteEdge(edge),
              insertion ? added.insertEdge(edge) : added.deleteEdge(edge));
    EXPECT_EQ(original.components(), added.components());
    EXPECT_TRUE(added.validate());
    for (Vertex source : graph.vertices)
      for (Vertex target : graph.vertices)
        EXPECT_EQ(original.connected(source, target),
                  added.connected(source, target));
  }
  EXPECT_EQ(added.statistics().cycle_rebuilds, 0u);
}

TEST_P(PrimaryComponentIntegrationTest,
       CountsComplementaryOrientationsAndPreservesIndices) {
  PrimaryComponentSolver solver(PrimaryComponentEdgeSemantics::ReferenceCounted,
                                GetParam());
  const Vertex low = std::numeric_limits<Vertex>::min();
  const Vertex high = std::numeric_limits<Vertex>::max();
  const Label label = std::numeric_limits<Label>::max();
  ASSERT_TRUE(solver.insertEdge({low, 0, label}));
  ASSERT_TRUE(solver.insertEdge({high, 0, label}));
  const auto first = solver.vertexIndex(low);
  const auto second = solver.vertexIndex(high);
  EXPECT_TRUE(solver.connectedByIndex(first, second));
  EXPECT_TRUE(solver.insertEdge({0, low, label, Parenthesis::Close}));
  EXPECT_EQ(solver.edgeMultiplicity({low, 0, label}), 2u);
  EXPECT_TRUE(solver.deleteEdge({low, 0, label}));
  EXPECT_TRUE(solver.connected(low, high));
  EXPECT_TRUE(solver.deleteEdge({0, low, label, Parenthesis::Close}));
  EXPECT_FALSE(solver.connectedByIndex(first, second));
  EXPECT_FALSE(solver.deleteEdge({low, 0, label}));
  solver.addVertex(42);
  PrimaryComponentSolver moved(std::move(solver));
  EXPECT_EQ(moved.vertexAt(first), low);
  EXPECT_EQ(moved.vertexAt(second), high);
  EXPECT_TRUE(moved.validate());
}

TEST_P(PrimaryComponentIntegrationTest, UsesActualLotusParsersAndGraphExport) {
  std::istringstream dot("digraph G {\n10->30[label=\"op--7\"];\n"
                         "20->30[label=\"op--7\"];\n}\n");
  PrimaryComponentSolver solver(parseDot(dot),
                                PrimaryComponentEdgeSemantics::Set, GetParam());
  ASSERT_TRUE(solver.connected(10, 20));
  std::istringstream sequence("D 30 20 cp--7\nA 40 30 op--7\n");
  for (const auto &update : parseUpdates(sequence))
    EXPECT_TRUE(solver.apply(update));
  EXPECT_FALSE(solver.connected(10, 20));
  EXPECT_TRUE(solver.connected(10, 40));
  WeightedQuotientSolver original(solver.graph());
  EXPECT_EQ(original.components(), solver.components());
  PrimaryComponentSolver restored(
      solver.graph(), PrimaryComponentEdgeSemantics::Set, GetParam());
  EXPECT_EQ(restored.components(), solver.components());
  EXPECT_TRUE(restored.validate());
}

INSTANTIATE_TEST_SUITE_P(
    Backends, PrimaryComponentIntegrationTest,
    ::testing::Values(PrimaryComponentConnectivityBackend::Deterministic,
                      PrimaryComponentConnectivityBackend::HDT));
} // namespace
