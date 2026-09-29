#include "CFL/Classical/Solvers/Engines/Skewed/InputBridge.h"
#include "CFL/Classical/Solvers/Engines/Skewed/SkewedTabulation.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <set>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

using NamedEdge = std::tuple<std::string, NodeId, NodeId>;
namespace sk = skewed;

std::set<NamedEdge> relationEdges(const SolverSession &session,
                                  const Grammar &grammar) {
  std::set<NamedEdge> result;
  for (const RelationEdge &edge : session.relation().edges()) {
    result.emplace(grammar.symbolName(edge.symbol), edge.source, edge.target);
  }
  return result;
}

std::set<NamedEdge> solveWith(SolverBackend backend, LabeledGraph graph,
                              const Grammar &grammar) {
  SolverSession session(graph, grammar, backend);
  session.solve();
  session.relation().forEachSuccessor(grammar.startSymbolId(), 0, [](NodeId) {
  }); // Exercise the adapter's streaming relation surface.
  return relationEdges(session, grammar);
}

TEST(SkewedTabulationCoreTest, UsesDynamicPropagatingEdges) {
  const sk::Grammar grammar{
      0, {false, true}, {sk::Rule::unary(0, 1), sk::Rule::binary(0, 0, 0)}};
  const sk::Graph graph{{0, 1, 2, 3}, {{0, 1, 1}, {1, 1, 2}, {2, 1, 3}}};

  const sk::Result result = sk::solve(grammar, graph);
  EXPECT_TRUE(result.contains(0, 0, 3));
  EXPECT_GT(result.stats().dynamic_eligible_symbols, 0u);
  EXPECT_GT(result.stats().dynamic_pe_insertions, 0u);
}

TEST(SkewedTabulationCoreTest, BridgeRestoresExternalSymbols) {
  sk::InputBridge<std::string> bridge;
  bridge.declareSymbol("S", false);
  bridge.declareSymbol("a", true);
  bridge.setStart("S");
  bridge.addUnary("S", "a");
  bridge.addTerminalEdge(10, "a", 20);

  const sk::Result result = bridge.solve();
  std::set<std::tuple<sk::Node, std::string, sk::Node>> facts;
  bridge.exportFacts(
      result, [&](sk::Node source, const std::string &symbol, sk::Node target) {
        facts.emplace(source, symbol, target);
      });
  EXPECT_NE(facts.count({10, "S", 20}), 0u);
  EXPECT_NE(facts.count({10, "a", 20}), 0u);
}

TEST(SkewedTabulationCoreTest, ConservativelySkewsTargetOnlyGrammar) {
  // S -> P C; C -> C C | b; P -> a.
  const sk::Grammar grammar{0,
                            {false, false, false, true, true},
                            {sk::Rule::binary(0, 2, 1),
                             sk::Rule::binary(1, 1, 1), sk::Rule::unary(1, 4),
                             sk::Rule::unary(2, 3)}};
  const sk::Graph graph{{0, 1, 2, 3}, {{0, 3, 1}, {1, 4, 2}, {2, 4, 3}}};
  sk::Options options;
  options.scope = sk::Scope::TargetsOnly;
  options.static_mode = sk::StaticMode::Conservative;

  const sk::Result result = sk::solve(grammar, graph, options);
  EXPECT_TRUE(result.contains(0, 0, 3));
  EXPECT_FALSE(result.rewrites().empty());
  EXPECT_THROW(result.contains(0, 1, 3), std::invalid_argument);
}

TEST(SkewedTabulationSessionTest, MatchesCompleteLotusRelationAndUpdates) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  S -> A B | <epsilon>; A -> a; B -> b;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addVertex("isolated");

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar, SolverBackend::SparseSet);
  SolverSession skewed_session(graph, grammar, SolverBackend::Skewed);
  EXPECT_EQ(skewed_session.solve().relation_edges,
            baseline.solve().relation_edges);
  EXPECT_EQ(solveWith(SolverBackend::Skewed, graph, grammar),
            solveWith(SolverBackend::SparseSet, graph, grammar));

  const NodeId n2 = skewed_session.addNode("n2");
  const NodeId baseline_n2 = baseline.addNode("n2");
  ASSERT_EQ(n2, baseline_n2);
  EXPECT_TRUE(skewed_session.addTerminalEdge(1, n2, "b"));
  EXPECT_TRUE(baseline.addTerminalEdge(1, baseline_n2, "b"));
  const auto skewed_stats = skewed_session.solve();
  baseline.solve();
  EXPECT_TRUE(skewed_session.contains(0, n2, "S"));
  EXPECT_TRUE(skewed_session.contains(n2, n2, "S"));
  EXPECT_EQ(relationEdges(skewed_session, grammar),
            relationEdges(baseline, grammar));
  EXPECT_GT(skewed_stats.processed_work_items, 0u);

  const auto unchanged = skewed_session.solve();
  EXPECT_EQ(unchanged.added_edges, 0u);
  EXPECT_EQ(unchanged.processed_work_items, 0u);
  EXPECT_EQ(unchanged.duplicate_edges, 0u);
}

TEST(SkewedTabulationSessionTest, AcceptsMigratedNonterminalAxioms) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a\nVariables:\n  S\n"
                             "Productions:\n  S -> S S;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "S");
  graph.addEdge("n1", "n2", "S");

  EXPECT_EQ(solveWith(SolverBackend::Skewed, graph, grammar),
            solveWith(SolverBackend::SparseSet, graph, grammar));
  EXPECT_NE(solveWith(SolverBackend::Skewed, graph, grammar).count({"S", 0, 2}),
            0u);
}

TEST(SkewedTabulationSessionTest, HonorsProjectedTargetsAndFollowMetadata) {
  const Grammar grammar = Grammar::parseFromText("Production:\n"
                                                 "S\tP\tb\n"
                                                 "P\ta\n\n"
                                                 "Insert:\nS\n\n"
                                                 "Follow:\nP\n\n"
                                                 "Count:\nS\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "b");

  SolverOptions options;
  options.backend = SolverBackend::Skewed;
  options.skewed.scope = sk::Scope::TargetsOnly;
  options.skewed.targets = {grammar.symbolId("S")};
  options.skewed.propagating_symbols =
      std::vector<sk::Symbol>{grammar.symbolId("P")};
  SolverSession session(graph, grammar, options);
  const ReachabilityStats stats = session.solve();

  EXPECT_TRUE(
      session.contains(graph.vertexId("n0"), graph.vertexId("n2"), "S"));
  EXPECT_FALSE(
      session.contains(graph.vertexId("n0"), graph.vertexId("n1"), "P"));
  EXPECT_EQ(stats.skewed_propagating_symbols, 1u);
  EXPECT_EQ(stats.skewed_propagating_facts, 1u);
  EXPECT_EQ(stats.skewed_inserted_summary_edges, 1u);
  EXPECT_EQ(stats.skewed_output_facts, 1u);
}

} // namespace
} // namespace lotus::cfl::classical
