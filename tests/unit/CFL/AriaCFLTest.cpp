#include "CFL/Aria/CFLSolver.h"
#include "CFL/Aria/CNF.h"
#include "CFL/Aria/Grammar.h"
#include "CFL/Aria/Graph.h"
#include "CFL/Aria/SCSolver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace lotus::cfl::aria {
namespace {

std::filesystem::path createTempFile(const std::string &name,
                                     const std::string &content) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream output(path);
  output << content;
  return path;
}

} // namespace

TEST(AriaGrammarTest, ParsesAndNormalizesDemoGrammar) {
  const auto grammar_path = createTempFile(
      "lotus_aria_grammar.txt", "Terminals:\n"
                                "  dbar d abar a\n"
                                "Variables:\n"
                                "  M V\n"
                                "Start:\n"
                                "  M\n"
                                "Productions:\n"
                                "  M -> dbar V d;\n"
                                "  V -> ( M ? abar ) * M ? ( a M ? ) *;\n");

  const auto grammar = Grammar::parseFromFile(grammar_path.string());

  EXPECT_FALSE(grammar.productions().empty());
  EXPECT_FALSE(grammar.nullableSymbols().empty());
  EXPECT_NE(grammar.binaryByFirst().find("dbar"),
            grammar.binaryByFirst().end());
}

TEST(AriaGraphTest, ParsesDotGraphWithMatrixAndPagModes) {
  const auto graph_path =
      createTempFile("lotus_aria_graph.dot", "digraph \"PEG\" {\n"
                                             "  NodeA [shape=circle];\n"
                                             "  NodeB [shape=circle];\n"
                                             "  NodeA -> NodeB[color=black];\n"
                                             "  NodeB -> NodeA[color=red];\n"
                                             "}\n");

  const auto matrix_graph =
      LabeledGraph::parseFromFile(graph_path.string(), GraphMode::Matrix);
  EXPECT_TRUE(matrix_graph.hasEdge(matrix_graph.vertexId("NodeA"),
                                   matrix_graph.vertexId("NodeB"), "a"));
  EXPECT_TRUE(matrix_graph.hasEdge(matrix_graph.vertexId("NodeB"),
                                   matrix_graph.vertexId("NodeA"), "abar"));

  const auto pag_graph =
      LabeledGraph::parseFromFile(graph_path.string(), GraphMode::PAGMatrix);
  EXPECT_TRUE(pag_graph.hasEdge(pag_graph.vertexId("NodeB"),
                                pag_graph.vertexId("NodeA"), "d"));
  EXPECT_TRUE(pag_graph.hasEdge(pag_graph.vertexId("NodeA"),
                                pag_graph.vertexId("NodeB"), "dbar"));
}

TEST(AriaCFLSolverTest, DerivesReachableLabels) {
  auto graph = LabeledGraph{};
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "b");

  const auto grammar_path =
      createTempFile("lotus_aria_solver_grammar.txt", "Terminals:\n"
                                                      "  a b\n"
                                                      "Variables:\n"
                                                      "  S A B\n"
                                                      "Start:\n"
                                                      "  S\n"
                                                      "Productions:\n"
                                                      "  A -> a;\n"
                                                      "  B -> b;\n"
                                                      "  S -> A B;\n");

  const auto grammar = Grammar::parseFromFile(grammar_path.string());
  const CFLSolver solver;
  const auto stats = solver.solve(graph, grammar);

  EXPECT_TRUE(graph.hasEdge(graph.vertexId("n0"), graph.vertexId("n2"), "S"));
  EXPECT_GT(stats.classical_iterations, 0U);
}

TEST(AriaSCSolverTest, ProducesConstraintStatistics) {
  auto graph = LabeledGraph{};
  graph.addEdge("n0", "n1", "a");

  const auto grammar_path =
      createTempFile("lotus_aria_sc_grammar.txt", "Terminals:\n"
                                                  "  a\n"
                                                  "Variables:\n"
                                                  "  A\n"
                                                  "Start:\n"
                                                  "  A\n"
                                                  "Productions:\n"
                                                  "  A -> a;\n");

  const auto grammar = Grammar::parseFromFile(grammar_path.string());
  const SCSolver solver;
  const auto stats = solver.solve(graph, grammar);

  EXPECT_GT(stats.constraint_variables, 0U);
  EXPECT_GT(stats.set_variables, 0U);
  EXPECT_GT(stats.classical_iterations, 0U);
}

TEST(AriaCNFTest, AppliesStbduPipeline) {
  const auto grammar_path =
      createTempFile("lotus_aria_cnf.txt", "Terminals:\n"
                                           "  a b\n"
                                           "Variables:\n"
                                           "  S A\n"
                                           "Productions:\n"
                                           "  S -> A b a;\n"
                                           "  A -> a | e;\n");

  const auto grammar = CNFGrammar::transformToSTBDU(grammar_path.string());

  EXPECT_FALSE(grammar.productions().empty());
  EXPECT_TRUE(
      std::any_of(grammar.productions().begin(), grammar.productions().end(),
                  [](const CNFRule &rule) { return rule.lhs == "S0"; }));
  EXPECT_TRUE(
      std::all_of(grammar.productions().begin(), grammar.productions().end(),
                  [](const CNFRule &rule) { return !rule.rhs.empty(); }));
}

} // namespace lotus::cfl::aria
