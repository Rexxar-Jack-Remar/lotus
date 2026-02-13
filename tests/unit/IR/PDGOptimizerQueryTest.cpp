#include "IR/PDG/Analysis/MotionLegality.h"
#include "IR/PDG/Analysis/SchedulingQuery.h"

#include "llvm/IR/Module.h"

#include <gtest/gtest.h>

#include <memory>

using namespace pdg;

namespace {

class TestGraph final : public GenericGraph {
public:
  void build(llvm::Module &M) override { (void)M; }
};

class PDGOptimizerQueryTest : public ::testing::Test {
protected:
  Node *addNode(GraphNodeType type = GraphNodeType::INST_OTHER) {
    nodes.emplace_back(std::make_unique<Node>(type));
    Node *n = nodes.back().get();
    graph.addNode(*n);
    return n;
  }

  void addEdge(Node *src, Node *dst, EdgeType type) {
    ASSERT_NE(src, nullptr);
    ASSERT_NE(dst, nullptr);
    edges.emplace_back(std::make_unique<Edge>(src, dst, type));
    Edge *e = edges.back().get();
    src->addOutEdge(*e);
    dst->addInEdge(*e);
    graph.addEdge(*e);
  }

  TestGraph graph;
  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<std::unique_ptr<Edge>> edges;
};

} // namespace

TEST_F(PDGOptimizerQueryTest, MotionEarlierBlockedByDependence) {
  Node *anchor = addNode();
  Node *moving = addNode();
  addEdge(anchor, moving, EdgeType::DATA_DEF_USE);

  MotionLegalityQuery q(graph);
  auto result = q.canMoveEarlier(*moving, *anchor);

  EXPECT_FALSE(result.legal);
  EXPECT_FALSE(result.blocking_path.empty());
}

TEST_F(PDGOptimizerQueryTest, MotionLaterBlockedByDependence) {
  Node *moving = addNode();
  Node *anchor = addNode();
  addEdge(moving, anchor, EdgeType::DATA_RAW);

  MotionLegalityQuery q(graph);
  auto result = q.canMoveLater(*moving, *anchor);

  EXPECT_FALSE(result.legal);
  EXPECT_FALSE(result.blocking_path.empty());
}

TEST_F(PDGOptimizerQueryTest, MotionAllowedWhenIndependent) {
  Node *anchor = addNode();
  Node *moving = addNode();

  MotionLegalityQuery q(graph);
  auto result = q.canMoveEarlier(*moving, *anchor);

  EXPECT_TRUE(result.legal);
}

TEST_F(PDGOptimizerQueryTest, SchedulingIndependenceWitnesses) {
  Node *a = addNode();
  Node *b = addNode();
  Node *mid = addNode();
  addEdge(a, mid, EdgeType::DATA_DEF_USE);
  addEdge(mid, b, EdgeType::DATA_DEF_USE);

  SchedulingQuery sq(graph);
  auto dep = sq.independent(*a, *b);
  auto indep = sq.independent(*b, *a);

  EXPECT_FALSE(dep.independent);
  EXPECT_FALSE(dep.witness_path_ab.empty());
  EXPECT_FALSE(indep.independent);
  EXPECT_FALSE(indep.witness_path_ba.empty());
}

TEST_F(PDGOptimizerQueryTest, SchedulingReadySetAndLevels) {
  Node *a = addNode();
  Node *b = addNode();
  Node *c = addNode();
  addEdge(a, b, EdgeType::DATA_DEF_USE);
  addEdge(a, c, EdgeType::DATA_DEF_USE);

  SchedulingQuery::NodeSet region = {a, b, c};
  SchedulingQuery::NodeSet scheduled;

  SchedulingQuery sq(graph);
  auto ready0 = sq.readySet(region, scheduled);
  EXPECT_EQ(ready0.size(), 1u);
  EXPECT_TRUE(ready0.count(a));

  scheduled.insert(a);
  auto ready1 = sq.readySet(region, scheduled);
  EXPECT_EQ(ready1.size(), 2u);
  EXPECT_TRUE(ready1.count(b));
  EXPECT_TRUE(ready1.count(c));

  auto levels = sq.topologicalLevels(region);
  ASSERT_EQ(levels.size(), 2u);
  EXPECT_EQ(levels[0].size(), 1u);
  EXPECT_EQ(levels[1].size(), 2u);
}

TEST_F(PDGOptimizerQueryTest, SchedulingCriticalPathAndSCC) {
  Node *a = addNode();
  Node *b = addNode();
  Node *c = addNode();
  addEdge(a, b, EdgeType::DATA_DEF_USE);
  addEdge(b, c, EdgeType::DATA_DEF_USE);

  SchedulingQuery::NodeSet region = {a, b, c};
  SchedulingQuery sq(graph);

  EXPECT_EQ(sq.criticalPathLength(region), 2u);

  addEdge(c, b, EdgeType::DATA_DEF_USE);
  auto sccs = sq.stronglyConnectedComponents(region);
  EXPECT_GE(sccs.size(), 2u);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
