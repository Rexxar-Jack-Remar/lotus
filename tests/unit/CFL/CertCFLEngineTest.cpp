#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Solvers/Engines/CERT/CertCFLEngine.h"
#include "CFL/Classical/Solvers/SolverSession.h"
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace lotus::cfl::classical {
namespace {
using Fact = std::tuple<SymbolId, NodeId, NodeId>;
std::set<Fact> collect(const Relation &relation) {
  std::set<Fact> facts;
  relation.visitEdges([&](const RelationEdge &e) {
    EXPECT_TRUE(facts.emplace(e.symbol, e.source, e.target).second);
    return true;
  });
  EXPECT_EQ(facts.size(), relation.edgeCount());
  return facts;
}
const Grammar &starGrammar() {
  static const auto grammar = Grammar::parseFromText(
      "Start:\n S\nTerminal:\n a\nVariables:\n S\nProductions:\n"
      " S -> S S | a | <epsilon>;\n");
  return grammar;
}
void compareFresh(const LabeledGraph &graph, const Grammar &grammar,
                  const Relation &result) {
  auto copy = graph;
  SolverSession reference(copy, grammar, SolverBackend::SparseSet);
  reference.solve();
  EXPECT_EQ(collect(result), collect(reference.relation()));
  for (std::size_t a = 0; a < grammar.symbolCount(); ++a) {
    const auto symbol = static_cast<SymbolId>(a);
    EXPECT_EQ(result.edgeCount(symbol), reference.relation().edgeCount(symbol));
    for (NodeId u = 0; u < graph.vertexCount(); ++u) {
      std::set<NodeId> out, in;
      EXPECT_TRUE(result.visitSuccessors(symbol, u, [&](NodeId v) {
        EXPECT_TRUE(out.insert(v).second); return true;
      }));
      EXPECT_TRUE(result.visitPredecessors(symbol, u, [&](NodeId v) {
        EXPECT_TRUE(in.insert(v).second); return true;
      }));
      for (NodeId v = 0; v < graph.vertexCount(); ++v) {
        EXPECT_EQ(result.contains(symbol, u, v), reference.relation().contains(symbol, u, v));
        EXPECT_EQ(out.count(v) != 0, reference.relation().contains(symbol, u, v));
        EXPECT_EQ(in.count(v) != 0, reference.relation().contains(symbol, v, u));
      }
    }
  }
}
} // namespace

TEST(CertCFLSessionTest, BackendNameRoundTrips) {
  EXPECT_EQ(parseSolverBackend("cert-cfl"), SolverBackend::CertCFL);
  EXPECT_STREQ(solverBackendName(SolverBackend::CertCFL), "cert-cfl");
}
TEST(CertCFLSessionTest, MatchesSparseSetAndStreamsWithoutDuplicates) {
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a"); graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n0", "a"); graph.addVertex("isolated");
  const auto &grammar = starGrammar();
  SolverSession session(graph, grammar, SolverBackend::CertCFL);
  EXPECT_EQ(session.relation().edgeCount(), 0u);
  session.solve(); compareFresh(graph, grammar, session.relation());
  std::size_t calls = 0;
  EXPECT_FALSE(session.relation().visitEdges([&](const RelationEdge &) {
    ++calls; return false;
  }));
  EXPECT_EQ(calls, 1u);
}
TEST(CertCFLSessionTest, MonotoneUpdatesExtendCompletedState) {
  LabeledGraph graph; graph.addEdge("n0", "n1", "a");
  const auto &grammar = starGrammar();
  SolverSession session(graph, grammar, SolverBackend::CertCFL);
  session.solve(); const auto oldCount = session.relation().edgeCount();
  const auto fresh = session.addNode("n2");
  EXPECT_TRUE(session.addTerminalEdge(1, fresh, "a"));
  EXPECT_FALSE(session.contains(0, fresh, "S"));
  EXPECT_FALSE(session.contains(fresh, fresh, "S"));
  EXPECT_EQ(session.relation().edgeCount(), oldCount);
  const auto stats = session.solve();
  EXPECT_TRUE(session.contains(0, fresh, "S"));
  EXPECT_TRUE(session.contains(fresh, fresh, "S"));
  EXPECT_EQ(stats.added_edges, stats.relation_edges - oldCount);
  EXPECT_EQ(stats.cert_cfl_levels, 1u);
  compareFresh(graph, grammar, session.relation());
  const auto noOp = session.solve();
  EXPECT_EQ(noOp.added_edges, 0u); EXPECT_EQ(noOp.processed_work_items, 0u);
  EXPECT_EQ(noOp.cert_cfl_levels, 0u);
}
TEST(CertCFLSessionTest, EmptyGraphAndNullableNodes) {
  const auto &grammar = starGrammar(); LabeledGraph graph;
  SolverSession session(graph, grammar, SolverBackend::CertCFL);
  EXPECT_EQ(session.solve().relation_edges, 0u);
  const auto node = session.addNode("isolated"); session.solve();
  EXPECT_TRUE(session.contains(node, node, "S"));
  compareFresh(graph, grammar, session.relation());
}
TEST(CertCFLSessionTest, ActiveTileLimitThrows) {
  LabeledGraph graph; graph.addEdge("n0", "n1", "a"); graph.addEdge("n1", "n2", "a");
  SolverOptions options; options.backend = SolverBackend::CertCFL;
  options.cert_cfl.max_tiles = 1;
  SolverSession session(graph, starGrammar(), options);
  EXPECT_THROW(session.solve(), engines::cert::ResourceLimit);
}
TEST(CertCFLSessionTest, CountSymbolsAreAnOffDiagonalUnion) {
  const auto grammar = Grammar::parseFromText(
      "Production:\nS\ta\nS\nT\tb\nT\nInsert:\nS\nT\nFollow:\n\nCount:\nS\nT\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a"); graph.addEdge("n0", "n1", "b");
  graph.addEdge("n0", "n2", "b"); graph.addEdge("n2", "n2", "a");
  SolverSession session(graph, grammar, SolverBackend::CertCFL);
  EXPECT_EQ(session.solve().count_symbol_edges, 2u);
  compareFresh(graph, grammar, session.relation());
}
TEST(CertCFLSessionTest, UnsupportedUnidirectionalModeIsRejected) {
  LabeledGraph graph;
  SolverOptions options; options.backend = SolverBackend::CertCFL; options.unidirectional = true;
  EXPECT_THROW((SolverSession(graph, starGrammar(), options)), std::invalid_argument);
}
TEST(CertCFLSessionTest, ExternalGraphMutationIsStillDetected) {
  LabeledGraph graph; graph.addVertex("n0");
  SolverSession session(graph, starGrammar(), SolverBackend::CertCFL);
  graph.addVertex("outside"); EXPECT_THROW(session.solve(), std::logic_error);
}
TEST(CertCFLEngineTest, ActiveTileLimitThrowsDuringUpdate) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n S\nTerminal:\n a\nVariables:\n S\nProductions:\n S -> a;\n");
  engines::cert::Options options; options.max_tiles = 2;
  engines::CertCFLEngine engine(grammar, 2, options);
  const auto a = grammar.symbolId("a");
  EXPECT_TRUE(engine.add(a, 0, 1)); engine.solve();
  EXPECT_EQ(engine.edgeCount(), 2u);
  EXPECT_TRUE(engine.add(a, 1, 0));
  EXPECT_THROW(engine.solve(), engines::cert::ResourceLimit);
}
TEST(CertCFLSessionTest, NonterminalSeedsAreNotDiscarded) {
  const auto &grammar = starGrammar(); LabeledGraph graph;
  graph.addEdge("n0", "n1", "S");
  SolverSession session(graph, grammar, SolverBackend::CertCFL);
  session.solve(); compareFresh(graph, grammar, session.relation());
}
TEST(CertCFLSessionTest, RandomizedIncrementalComparisons) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n S\nTerminal:\n a b\nVariables:\n S A B\nProductions:\n"
      " A -> a; B -> b; S -> S S | A B | B A | A | <epsilon>;\n");
  std::mt19937 random(20260921);
  for (unsigned trial = 0; trial < 60; ++trial) {
    SCOPED_TRACE(trial); LabeledGraph graph;
    for (unsigned u = 0; u < 6; ++u) graph.addVertex("n" + std::to_string(u));
    SolverOptions options; options.backend = SolverBackend::CertCFL;
    SolverSession session(graph, grammar, options);
    for (unsigned round = 0; round < 4; ++round) {
      SCOPED_TRACE(round);
      for (unsigned i = 0; i < 5; ++i)
        session.addTerminalEdge(random() % graph.vertexCount(), random() % graph.vertexCount(),
                                random() % 2 ? "a" : "b");
      session.solve(); compareFresh(graph, grammar, session.relation());
    }
  }
}
} // namespace lotus::cfl::classical
