#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotientEngine.h"

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

using endpoint::Id;
using endpoint::Problem;
using endpoint::Rule;
using endpoint::Solver;
using NamedEdge = std::tuple<std::string, std::size_t, std::size_t>;

const Grammar &starGrammar() {
  static const Grammar grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a | <epsilon>;\n");
  return grammar;
}

std::set<NamedEdge> sessionEdges(SolverSession &session,
                                 const Grammar &grammar) {
  std::set<NamedEdge> result;
  for (const RelationEdge &edge : session.relation().edges()) {
    result.emplace(grammar.symbolName(edge.symbol), edge.source, edge.target);
  }
  return result;
}

std::set<NamedEdge> referenceClosure(const LabeledGraph &graph,
                                     const Grammar &grammar) {
  const std::size_t node_count = graph.vertexCount();
  const std::size_t symbol_count = grammar.symbolCount();
  std::vector<std::vector<std::vector<bool>>> relation(
      symbol_count, std::vector<std::vector<bool>>(
                        node_count, std::vector<bool>(node_count, false)));
  for (const LabeledEdge &edge : graph.edges()) {
    relation[grammar.symbolId(edge.label)][edge.source][edge.target] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[head, rules] : grammar.productions()) {
      const SymbolId lhs = grammar.symbolId(head);
      for (const auto &rule : rules) {
        if (rule.size() == 1 && rule.front() == Grammar::kEpsilonSymbol) {
          for (std::size_t node = 0; node < node_count; ++node) {
            if (!relation[lhs][node][node]) {
              relation[lhs][node][node] = true;
              changed = true;
            }
          }
        } else if (rule.size() == 1) {
          const SymbolId rhs = grammar.symbolId(rule.front());
          for (std::size_t source = 0; source < node_count; ++source) {
            for (std::size_t target = 0; target < node_count; ++target) {
              if (relation[rhs][source][target] &&
                  !relation[lhs][source][target]) {
                relation[lhs][source][target] = true;
                changed = true;
              }
            }
          }
        } else if (rule.size() == 2) {
          const SymbolId first = grammar.symbolId(rule[0]);
          const SymbolId second = grammar.symbolId(rule[1]);
          for (std::size_t source = 0; source < node_count; ++source) {
            for (std::size_t middle = 0; middle < node_count; ++middle) {
              for (std::size_t target = 0; target < node_count; ++target) {
                if (relation[first][source][middle] &&
                    relation[second][middle][target] &&
                    !relation[lhs][source][target]) {
                  relation[lhs][source][target] = true;
                  changed = true;
                }
              }
            }
          }
        }
      }
    }
  }
  std::set<NamedEdge> result;
  for (std::size_t symbol = 0; symbol < symbol_count; ++symbol) {
    for (std::size_t source = 0; source < node_count; ++source) {
      for (std::size_t target = 0; target < node_count; ++target) {
        if (relation[symbol][source][target]) {
          result.emplace(grammar.symbolName(symbol), source, target);
        }
      }
    }
  }
  return result;
}

} // namespace

TEST(EndpointQuotientCoreTest, ArtifactExampleBinaryRule) {
  Problem problem{5,
                  3,
                  {{0, 0, 2}, {1, 0, 2}, {2, 1, 3}, {2, 1, 4}},
                  {Rule::binary(2, 0, 1)}};
  Solver solver(std::move(problem));
  solver.solve();
  EXPECT_TRUE(solver.contains(0, 0, 2));
  EXPECT_TRUE(solver.contains(2, 0, 4));
  EXPECT_TRUE(solver.contains(2, 1, 4));
  EXPECT_FALSE(solver.contains(2, 0, 2));
  EXPECT_FALSE(solver.contains(2, 1, 2));
  EXPECT_EQ(solver.statistics().cells, 3u);
  EXPECT_EQ(solver.statistics().logical_facts, 8u);
}

TEST(EndpointQuotientCoreTest, NullableDiagonalStaysSymbolic) {
  Problem problem{3, 2, {{0, 0, 1}}, {Rule::epsilon(1), Rule::unary(1, 0)}};
  Solver solver(std::move(problem));
  solver.solve();
  EXPECT_TRUE(solver.isNullable(1));
  EXPECT_TRUE(solver.contains(1, 0, 0));
  EXPECT_TRUE(solver.contains(1, 1, 1));
  EXPECT_TRUE(solver.contains(1, 2, 2));
  EXPECT_TRUE(solver.contains(1, 0, 1));
  EXPECT_FALSE(solver.contains(1, 0, 2));
}

TEST(EndpointQuotientSessionTest, MatchesReferenceClosure) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");

  const auto expected = referenceClosure(graph, grammar);
  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  session.solve();
  EXPECT_EQ(sessionEdges(session, grammar), expected);
  EXPECT_TRUE(
      session.contains(graph.vertexId("n0"), graph.vertexId("n3"), "S"));
}

TEST(EndpointQuotientSessionTest, StarGraphCompresses) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  const std::size_t leaves = 100;
  for (std::size_t leaf = 1; leaf <= leaves; ++leaf) {
    graph.addEdge("hub", "leaf" + std::to_string(leaf), "a");
    graph.addEdge("leaf" + std::to_string(leaf), "hub", "a");
  }

  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  const ReachabilityStats stats = session.solve();
  EXPECT_EQ(sessionEdges(session, grammar), referenceClosure(graph, grammar));
  EXPECT_EQ(stats.endpoint_quotient_cells, 6u);
  EXPECT_GT(stats.endpoint_quotient_facts,
            static_cast<std::size_t>(leaves * leaves));
}

TEST(EndpointQuotientSessionTest, MatchesSparseSetBackend) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");
  graph.addEdge("n3", "n4", "a");

  SolverSession baseline(graph, grammar, SolverBackend::SparseSet);
  baseline.solve();
  SolverSession quotient(graph, grammar, SolverBackend::EndpointQuotient);
  quotient.solve();
  EXPECT_EQ(sessionEdges(quotient, grammar), sessionEdges(baseline, grammar));
}

TEST(EndpointQuotientSessionTest, IncrementalTerminalEdgesResolve) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  graph.addVertex("n2");
  graph.addEdge("n0", "n1", "a");

  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  session.solve();
  EXPECT_TRUE(
      session.contains(graph.vertexId("n0"), graph.vertexId("n1"), "S"));
  EXPECT_FALSE(
      session.contains(graph.vertexId("n0"), graph.vertexId("n2"), "S"));

  session.addTerminalEdge(graph.vertexId("n1"), graph.vertexId("n2"), "a");
  session.solve();
  EXPECT_TRUE(
      session.contains(graph.vertexId("n0"), graph.vertexId("n2"), "S"));
  EXPECT_TRUE(
      session.contains(graph.vertexId("n0"), graph.vertexId("n1"), "S"));
}

TEST(EndpointQuotientSessionTest, EmptyGraphWithNullableGrammar) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  graph.addVertex("n0");
  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  session.solve();
  EXPECT_TRUE(session.contains(0, 0, "S"));
}

TEST(EndpointQuotientSessionTest,
     LargeClosureStaysCompressedAndStreamsQueries) {
  const auto &grammar = starGrammar();
  LabeledGraph graph;
  constexpr std::size_t leaves = 2048;
  for (std::size_t i = 0; i < leaves; ++i) {
    graph.addEdge("hub", "leaf" + std::to_string(i), "a");
    graph.addEdge("leaf" + std::to_string(i), "hub", "a");
  }
  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  const auto stats = session.solve();
  const auto &relation = session.relation();
  const auto symbol = grammar.symbolId("S");
  EXPECT_EQ(stats.endpoint_quotient_cells, 6u);
  EXPECT_EQ(relation.edgeCount(symbol), (leaves + 1) * (leaves + 1));
  EXPECT_LT(relation.estimatedPayloadBytes(), relation.edgeCount());
  const auto bytes = relation.estimatedPayloadBytes();
  std::set<NodeId> successors, predecessors;
  relation.forEachSuccessor(
      symbol, 1, [&](NodeId v) { EXPECT_TRUE(successors.insert(v).second); });
  relation.forEachPredecessor(
      symbol, 1, [&](NodeId v) { EXPECT_TRUE(predecessors.insert(v).second); });
  EXPECT_EQ(successors.size(), leaves + 1);
  EXPECT_EQ(predecessors, successors);
  std::size_t visits = 0;
  EXPECT_FALSE(relation.visitEdges(
      symbol, [&](const RelationEdge &) { return ++visits < 3; }));
  EXPECT_EQ(visits, 3u);
  visits = 0;
  EXPECT_FALSE(relation.visitSuccessors(symbol, 1, [&](NodeId) {
    ++visits;
    return false;
  }));
  EXPECT_EQ(visits, 1u);
  EXPECT_EQ(relation.estimatedPayloadBytes(), bytes);
}

TEST(EndpointQuotientSessionTest, SnapshotQueriesAndNoOpSolvesAcrossUpdates) {
  const auto &grammar = starGrammar();
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  EXPECT_EQ(session.relation().edgeCount(), 0u);
  session.solve();
  const auto old_count = session.relation().edgeCount();
  EXPECT_FALSE(session.addTerminalEdge(0, 1, "a"));
  const auto unchanged = session.solve();
  EXPECT_EQ(unchanged.added_edges, 0u);
  EXPECT_EQ(unchanged.duplicate_edges, 0u);
  EXPECT_EQ(unchanged.processed_work_items, 0u);
  EXPECT_EQ(unchanged.endpoint_quotient_preprocess_us, 0u);
  EXPECT_EQ(unchanged.endpoint_quotient_partitions_built, 0u);
  const auto added = session.addNode("n2");
  session.addTerminalEdge(1, added, "a");
  EXPECT_FALSE(session.contains(0, added, "S"));
  EXPECT_EQ(session.relation().edgeCount(), old_count);
  std::size_t visits = 0;
  session.relation().forEachSuccessor(grammar.symbolId("S"), added,
                                      [&](NodeId) { ++visits; });
  EXPECT_EQ(visits, 0u);
  const auto updated = session.solve();
  EXPECT_TRUE(session.contains(0, added, "S"));
  EXPECT_TRUE(session.contains(added, added, "S"));
  EXPECT_EQ(updated.added_edges, updated.relation_edges - old_count);
  EXPECT_EQ(sessionEdges(session, grammar), referenceClosure(graph, grammar));
}

TEST(EndpointQuotientCoreTest, RandomizedQueriesMatchConcreteFixedPoint) {
  using Fact = std::tuple<Id, Id, Id>;
  std::mt19937 random(0x4c4f5455);
  for (unsigned trial = 0; trial < 80; ++trial) {
    Problem p;
    p.nodes = random() % 8;
    p.symbols = 1 + random() % 5;
    for (unsigned i = 0; p.nodes && i < 18; ++i)
      p.edges.push_back(
          {random() % p.nodes, random() % p.symbols, random() % p.nodes});
    for (unsigned i = 0; i < 12; ++i) {
      const Id a = random() % p.symbols;
      const Id b = random() % p.symbols;
      const Id c = random() % p.symbols;
      switch (random() % 3) {
      case 0:
        p.rules.push_back(Rule::epsilon(a));
        break;
      case 1:
        p.rules.push_back(Rule::unary(a, b));
        break;
      default:
        p.rules.push_back(Rule::binary(a, b, c));
        break;
      }
    }
    std::set<Fact> expected;
    for (auto e : p.edges)
      expected.emplace(e.symbol, e.source, e.target);
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto r : p.rules)
        for (Id u = 0; u < p.nodes; ++u)
          for (Id v = 0; v < p.nodes; ++v) {
            bool derives = r.kind == Rule::Kind::Epsilon && u == v;
            if (r.kind == Rule::Kind::Unary)
              derives = expected.count({r.left, u, v});
            if (r.kind == Rule::Kind::Binary)
              for (Id m = 0; m < p.nodes; ++m)
                derives = derives || (expected.count({r.left, u, m}) &&
                                      expected.count({r.right, m, v}));
            if (derives)
              changed = expected.emplace(r.lhs, u, v).second || changed;
          }
    }
    for (auto mode :
         {endpoint::PartitionMode::Grammar, endpoint::PartitionMode::Global,
          endpoint::PartitionMode::Singleton}) {
      SCOPED_TRACE(trial);
      SCOPED_TRACE(static_cast<int>(mode));
      Solver solver(p, {mode});
      solver.solve();
      EXPECT_EQ(solver.statistics().logical_facts, expected.size());
      std::set<Fact> actual;
      std::set<std::pair<Id, Id>> off_diagonal;
      std::vector<Id> symbols;
      for (Id a = 0; a < p.symbols; ++a) {
        symbols.push_back(a);
        endpoint::Count count = 0, diagonal = 0;
        EXPECT_TRUE(solver.visitFacts(a, [&](Id u, Id v) {
          EXPECT_TRUE(actual.emplace(a, u, v).second);
          ++count;
          diagonal += u == v;
          if (u != v)
            off_diagonal.emplace(u, v);
          return true;
        }));
        EXPECT_EQ(solver.statistics().per_symbol[a].logical_facts, count);
        EXPECT_EQ(solver.statistics().per_symbol[a].diagonal_facts, diagonal);
        for (Id u = 0; u < p.nodes; ++u) {
          std::set<Id> out, in;
          solver.visitSuccessors(a, u, [&](Id v) {
            EXPECT_TRUE(out.insert(v).second);
            return true;
          });
          solver.visitPredecessors(a, u, [&](Id v) {
            EXPECT_TRUE(in.insert(v).second);
            return true;
          });
          for (Id v = 0; v < p.nodes; ++v) {
            EXPECT_EQ(solver.contains(a, u, v), expected.count({a, u, v}) != 0);
            EXPECT_EQ(out.count(v), expected.count({a, u, v}));
            EXPECT_EQ(in.count(v), expected.count({a, v, u}));
          }
        }
        EXPECT_EQ(solver.countOffDiagonalUnion(symbols), off_diagonal.size());
      }
      EXPECT_EQ(actual, expected);
    }
  }
}

TEST(EndpointQuotientCoreTest, SharedPlansAndRepeatedRefinementOutputs) {
  Problem p{7,
            5,
            {{0, 0, 2},
             {0, 0, 3},
             {1, 0, 2},
             {1, 0, 3},
             {6, 0, 2},
             {2, 1, 4},
             {2, 1, 5},
             {3, 1, 4},
             {3, 1, 5},
             {0, 2, 4},
             {1, 2, 5}},
            {Rule::binary(3, 0, 1), Rule::unary(3, 2), Rule::binary(4, 0, 1),
             Rule::unary(4, 2)}};
  Solver solver(p);
  solver.solve();
  const auto &stats = solver.statistics();
  EXPECT_EQ(stats.bridges_built, 1u);
  EXPECT_GT(stats.repeated_binary_outputs, 0u);
  for (Id a : {3u, 4u})
    for (Id u : {0u, 1u, 6u})
      for (Id v : {4u, 5u})
        EXPECT_TRUE(solver.contains(a, u, v));
  EXPECT_EQ(stats.per_symbol[3].logical_facts, 6u);
  EXPECT_EQ(stats.per_symbol[4].logical_facts, 6u);
}

TEST(EndpointQuotientCoreTest, EmptySymbolsSharePartitions) {
  Problem p{2000, 64, {}, {}};
  for (Id a = 1; a < p.symbols; ++a)
    p.rules.push_back(Rule::unary(a - 1, a));
  Solver solver(p);
  solver.solve();
  EXPECT_EQ(solver.statistics().partitions_built, 1u);
  EXPECT_EQ(solver.statistics().lifts_built, 1u);
  EXPECT_EQ(solver.statistics().logical_facts, 0u);
}

TEST(EndpointQuotientCoreTest, LargeSparseQuotientQueries) {
  Solver solver({4096, 1, {{1, 0, 2}, {2, 0, 3}}, {Rule::binary(0, 0, 0)}},
                {endpoint::PartitionMode::Singleton});
  solver.solve();
  EXPECT_EQ(solver.statistics().logical_facts, 3u);
  EXPECT_TRUE(solver.contains(0, 1, 3));
  EXPECT_FALSE(solver.contains(0, 3, 1));
  EXPECT_EQ(solver.countOffDiagonalUnion({0, 0}), 3u);
}

TEST(EndpointQuotientCoreTest, BatchedJoinsHandleWordBoundariesAndPromotion) {
  for (Id n : {63u, 64u, 65u, 127u, 129u, 320u}) {
    SCOPED_TRACE(n);
    Problem p{n, 2, {}, {Rule::unary(1, 0), Rule::binary(1, 1, 1)}};
    for (Id v = 1; v < n; ++v)
      p.edges.push_back({v - 1, 0, v});
    Solver solver(p);
    solver.solve();
    const auto &stats = solver.statistics();
    EXPECT_EQ(stats.per_symbol[1].logical_facts, n * (n - 1) / 2);
    EXPECT_EQ(stats.binary_joins, n * (n - 1) * (n - 2) / 6);
    EXPECT_GT(stats.binary_join_words, 0u);
    EXPECT_LT(stats.binary_propagations, stats.binary_joins / 5);
    for (Id u = 0; u < n; ++u)
      for (Id v = 0; v < n; ++v)
        EXPECT_EQ(solver.contains(1, u, v), u < v);
  }
}

TEST(EndpointQuotientSessionTest, CountSymbolsUseExactCompressedUnion) {
  const auto grammar =
      Grammar::parseFromText("Production:\nS\ta\nS\nT\tb\nT\n"
                             "Insert:\nS\nT\nFollow:\n\nCount:\nS\nT\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n0", "n1", "b");
  graph.addEdge("n0", "n2", "b");
  graph.addEdge("n2", "n2", "a");
  graph.addVertex("isolated");
  ASSERT_EQ(grammar.countSymbols().size(), 2u);
  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  EXPECT_EQ(session.solve().count_symbol_edges, 2u);
  EXPECT_EQ(session.solve().count_symbol_edges, 2u);
  session.addTerminalEdge(graph.vertexId("n1"), graph.vertexId("n2"), "a");
  EXPECT_EQ(session.solve().count_symbol_edges, 3u);
  EXPECT_EQ(sessionEdges(session, grammar), referenceClosure(graph, grammar));
}

TEST(EndpointQuotientSessionTest, StreamingQueriesAreBackendIndependent) {
  for (auto backend :
       {SolverBackend::SparseSet, SolverBackend::SparseBitVector,
        SolverBackend::Graspan, SolverBackend::Sqid, SolverBackend::Pearl,
        SolverBackend::Skewed, SolverBackend::Cat, SolverBackend::Iea,
        SolverBackend::IeaOcr, SolverBackend::TransitiveClosure,
        SolverBackend::Pocr, SolverBackend::HierarchicalPocr,
        SolverBackend::FullyOrdered, SolverBackend::EndpointQuotient}) {
    SCOPED_TRACE(solverBackendName(backend));
    const auto &grammar = starGrammar();
    LabeledGraph graph;
    graph.addEdge("n0", "n1", "a");
    graph.addEdge("n1", "n2", "a");
    SolverSession session(graph, grammar, backend);
    session.solve();
    const auto &r = session.relation();
    std::set<NamedEdge> streamed;
    EXPECT_TRUE(r.visitEdges([&](const RelationEdge &edge) {
      EXPECT_TRUE(streamed
                      .emplace(grammar.symbolName(edge.symbol), edge.source,
                               edge.target)
                      .second);
      return true;
    }));
    EXPECT_EQ(streamed, referenceClosure(graph, grammar));
    EXPECT_EQ(r.edges().size(), streamed.size());
    const auto a = grammar.symbolId("S");
    std::size_t visits = 0;
    EXPECT_FALSE(
        r.visitEdges(a, [&](const RelationEdge &) { return ++visits < 2; }));
    EXPECT_EQ(visits, 2u);
    visits = 0;
    EXPECT_FALSE(
        r.visitPredecessors(a, 2, [&](NodeId) { return ++visits < 2; }));
    EXPECT_EQ(visits, 2u);
    visits = 0;
    EXPECT_FALSE(r.visitSuccessors(a, 0, [&](NodeId) { return ++visits < 2; }));
    EXPECT_EQ(visits, 2u);
  }
}

} // namespace lotus::cfl::classical
