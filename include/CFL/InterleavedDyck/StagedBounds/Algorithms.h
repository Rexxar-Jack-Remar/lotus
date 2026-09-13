#pragma once

#include "CFL/InterleavedDyck/StagedBounds/Solver.h"

#include <optional>
#include <vector>

namespace lotus::cfl::interleaved_dyck::staged_bounds::detail {

inline constexpr unsigned MAX_PARITY_GROUPS = 4;

bool isParenthesis(LabelKind kind);
bool isBracket(LabelKind kind);
bool isOpen(LabelKind kind);
bool belongsTo(LabelKind kind, Alphabet alphabet);
Label openLabel(Alphabet alphabet, unsigned id);
Label closeLabel(Alphabet alphabet, unsigned id);
PairSet intersect(const PairSet &left, const PairSet &right);
std::vector<unsigned> labelIds(const Graph &graph, Alphabet alphabet);
std::vector<unsigned> matchedLabelIds(const Graph &graph, Alphabet alphabet);

Graph retainMatchedLabels(const Graph &graph);
Graph removeNotOnCandidatePaths(const Graph &graph, const PairSet &pairs);
Graph removeValueFlowUnreachable(const Graph &graph);
PairSet filterBracketPaths(const Graph &graph, const PairSet &pairs);
Vertex productVertex(Vertex vertex, std::size_t states, std::size_t state);
Graph valueFlowTransform(const Graph &graph);
PairSet filterValueFlowPairs(const PairSet &pairs);

struct ReachabilityRun {
  PairSet pairs;
  Graph used_edges;
};

ReachabilityRun
runProjected(const Graph &graph, Alphabet balanced, GrammarStrength strength,
             unsigned parity_groups, bool trace = false,
             bool factorized_tracing = false,
             const std::optional<Pair> &trace_pair = std::nullopt);
PairSet runCombined(const Graph &graph);

PairSet regularization(const Graph &graph, BenchmarkKind benchmark);
PairSet refinedWithCondensation(const Graph &graph,
                                const PairSet &underapproximation,
                                GrammarStrength strength,
                                unsigned parity_groups, BenchmarkKind benchmark,
                                bool factorized_tracing);
PairSet onDemand(const Graph &graph, const PairSet &underapproximation,
                 const PairSet &overapproximation, GrammarStrength strength,
                 unsigned parity_groups, BenchmarkKind benchmark,
                 bool factorized_tracing);

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::detail
