#include "CFL/Classical/Solvers/Engines/CAT/ContextAwareTabulation.h"
#include "CFL/Classical/Solvers/Engines/Common/InputBridge.h"
#include "CFL/Classical/Solvers/Engines/IEOCE/IterativeEpoch.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <set>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

namespace ca = cat;
namespace cm = common;
namespace ie = ieoce;
using NamedEdge = std::tuple<std::string, NodeId, NodeId>;

std::set<NamedEdge> solveWith(SolverBackend backend, LabeledGraph graph,
                              const Grammar &grammar) {
  SolverSession session(graph, grammar, backend);
  session.solve();
  std::set<NamedEdge> result;
  for (const RelationEdge &edge : session.relation().edges())
    result.emplace(grammar.symbolName(edge.symbol), edge.source, edge.target);
  return result;
}

cm::Grammar contractionGrammar() {
  // S=0, A=1, d=2, a=3. S absorbs A on both sides.
  cm::Grammar grammar;
  grammar.start = 0;
  grammar.terminal = {false, false, true, true};
  grammar.rules = {cm::Rule::binary(0, 1, 0), cm::Rule::binary(0, 0, 1),
                   cm::Rule::unary(0, 2), cm::Rule::binary(1, 1, 1),
                   cm::Rule::unary(1, 3)};
  return grammar;
}

TEST(CatCoreTest, PrunesAdjacencyUsingUsageContexts) {
  cm::Grammar grammar;
  grammar.start = 0;
  grammar.terminal = {false, false, false, true, true, true};
  grammar.rules = {cm::Rule::binary(0, 2, 5), cm::Rule::binary(1, 1, 4),
                   cm::Rule::binary(1, 1, 0), cm::Rule::epsilon(1),
                   cm::Rule::binary(2, 3, 1)};
  const cm::Graph graph{{0, 1, 2, 3, 4},
                        {{0, 3, 1}, {1, 4, 2}, {2, 4, 3}, {3, 5, 4}}};
  ca::Options options;
  options.propagating_symbols = false;
  options.dynamic_skewing = false;

  const ca::Result result = ca::solve(grammar, graph, options);
  EXPECT_TRUE(result.contains(0, 0, 4));
  EXPECT_EQ(result.stats.graph_degree, 13u);
  EXPECT_GT(result.stats.context_annotations, 0u);
}

TEST(IeoceCoreTest, IeaAndIeaOcrContractEligibleCycles) {
  const cm::Grammar grammar = contractionGrammar();
  const cm::Graph graph{{0, 1, 2}, {{0, 3, 1}, {1, 3, 0}, {1, 2, 2}}};

  for (ie::Variant variant : {ie::Variant::Iea, ie::Variant::IeaOcr}) {
    ie::Options options;
    options.variant = variant;
    options.transitive_symbol = 1;
    options.check_invariants = true;
    const ie::Result result = ie::solve(grammar, graph, options);
    EXPECT_TRUE(result.contains(0, 0, 2));
    EXPECT_EQ(result.stats.merged_nodes, 1u);
    EXPECT_FALSE(result.stats.ordinary_fallback);
    if (variant == ie::Variant::IeaOcr)
      EXPECT_GT(result.stats.meg_insertions, 0u);
  }
}

TEST(CommonEngineTest, InputBridgeExportsExternalSymbols) {
  cm::InputBridge<std::string> bridge;
  bridge.declareSymbol("S", false);
  bridge.declareSymbol("a", true);
  bridge.setStart("S");
  bridge.addUnary("S", "a");
  bridge.addTerminalEdge(10, "a", 20);
  ca::Options options;
  options.query.scope = cm::Scope::AllSymbols;
  const ca::Result result =
      ca::solve(bridge.grammar(), bridge.graph(), options);

  std::set<std::tuple<cm::Node, std::string, cm::Node>> facts;
  cm::exportFacts(
      bridge, result.reachability,
      [&](cm::Node source, const std::string &symbol, cm::Node target) {
        facts.emplace(source, symbol, target);
      });
  EXPECT_NE(facts.count({10, "S", 20}), 0u);
  EXPECT_NE(facts.count({10, "a", 20}), 0u);
}

TEST(BatchSolverSessionTest, AllNewBackendsMatchCompleteRelationAndUpdates) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  S -> A B | <epsilon>; A -> a; B -> b;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addVertex("isolated");
  const auto baseline = solveWith(SolverBackend::SparseSet, graph, grammar);

  for (SolverBackend backend :
       {SolverBackend::Cat, SolverBackend::Iea, SolverBackend::IeaOcr}) {
    EXPECT_EQ(solveWith(backend, graph, grammar), baseline)
        << solverBackendName(backend);
    LabeledGraph incremental_graph = graph;
    SolverSession session(incremental_graph, grammar, backend);
    session.solve();
    const NodeId n2 = session.addNode("n2");
    EXPECT_TRUE(session.addTerminalEdge(1, n2, "b"));
    EXPECT_TRUE(session.solve().processed_work_items > 0)
        << solverBackendName(backend);
    EXPECT_TRUE(session.contains(0, n2, "S"));
    EXPECT_TRUE(session.contains(n2, n2, "S"));
    const ReachabilityStats unchanged = session.solve();
    EXPECT_EQ(unchanged.processed_work_items, 0u);
    EXPECT_EQ(unchanged.added_edges, 0u);
  }
}

TEST(BatchSolverSessionTest, IeoceKeepsContractedResultsCompressed) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a\nVariables:\n  S\n"
                             "Productions:\n  S -> S S | a;\n");
  LabeledGraph graph;
  constexpr std::size_t nodes = 32;
  for (std::size_t node = 0; node < nodes; ++node)
    graph.addEdge("n" + std::to_string(node),
                  "n" + std::to_string((node + 1) % nodes), "a");

  for (SolverBackend backend : {SolverBackend::Iea, SolverBackend::IeaOcr}) {
    LabeledGraph backend_graph = graph;
    SolverSession session(backend_graph, grammar, backend);
    const ReachabilityStats stats = session.solve();
    EXPECT_EQ(stats.ieoce_merged_nodes, nodes - 1)
        << solverBackendName(backend);
    EXPECT_LT(stats.batch_stored_facts, stats.relation_edges)
        << solverBackendName(backend);
    EXPECT_TRUE(session.contains(7, 19, "S"));
    std::size_t visits = 0;
    EXPECT_FALSE(session.relation().visitEdges([&](const RelationEdge &) {
      ++visits;
      return false;
    }));
    EXPECT_EQ(visits, 1u);
  }
}

} // namespace
} // namespace lotus::cfl::classical
