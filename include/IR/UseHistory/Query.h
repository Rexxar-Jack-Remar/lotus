#ifndef LOTUS_IR_USEHISTORY_QUERY_H
#define LOTUS_IR_USEHISTORY_QUERY_H

#include "IR/UseHistory/FlowGraph.h"

namespace lotus {
namespace usehistory {

enum class ContextMode {
  Insensitive,
  /// All calls/returns on a witness must match; empty initial/final stack.
  Balanced,
  /// Any contiguous segment of a well-nested execution. Allows unmatched
  /// returns at the beginning and unmatched calls at the end, never mismatches.
  Realizable
};
enum class QueryStatus { Found, NotFound, Unknown };

/// Deterministic event automaton. May-events additionally have a no-effect
/// transition. Product construction happens before context-sensitive solving.
struct Automaton {
  ID states = 1;
  ID initial = 0;
  std::vector<ID> accepting = {0};
  std::function<ID(ID, Event)> transition;
  static Automaton ordered(std::vector<Event> sequence);
  static Automaton doubleFree();
  static Automaton useAfterFree();
};

struct Query {
  std::vector<FlowNodeID> sources;
  std::vector<FlowNodeID> sinks;
  /// Structural traps include endpoints and unconditionally block the node.
  std::vector<FlowNodeID> traps;
  /// Must events block; May events retain the no-effect alternative.
  Event trapEvents = Event::None;
  ContextMode context = ContextMode::Realizable;
  Automaton automaton;
  bool requireNonEmpty = false;
  /// Threads have no sequential call-stack interpretation. Ignoring a reached
  /// thread edge yields Unknown on a negative result, not a safety proof.
  bool includeThreadEdges = false;
  /// Optional, FIXED memory-object filter. Never intersects points-to sets
  /// globally along a taint path: different accesses can address different objects.
  std::optional<ObjectID> memoryObject;
  std::function<bool(const FlowEdge &)> edgeFilter;
  std::size_t maxProductStates = 100000;
  std::size_t maxSummaryPairs = 500000;
  std::size_t maxWork = 5000000;
  std::size_t maxWitnessEdges = 1000000;
};

struct QueryResult {
  QueryStatus status = QueryStatus::NotFound;
  std::vector<FlowNodeID> nodes;
  std::vector<FlowEdgeID> edges;
  std::vector<ID> automatonStates;
  bool witnessComplete = true;
  std::size_t productStates = 0;
  std::size_t summaryPairs = 0;
  std::string message;
  /// Found means a witness IN THIS ABSTRACTION, not a proof of concrete feasibility.
  bool found() const { return status == QueryStatus::Found; }
};

enum class CoverageStatus { SinkUnreachable, AllPathsTrapped, UntrappedPath, Unknown };
struct CoverageResult {
  CoverageStatus status = CoverageStatus::Unknown;
  QueryResult evidence;
};

class QueryEngine {
public:
  explicit QueryEngine(const FlowGraph &graph) : G(graph) {}
  QueryResult run(const Query &query) const;
  std::vector<QueryResult> runBatch(const std::vector<Query> &queries) const;
  /// Distinguishes vacuous coverage (sink unreachable even without traps).
  CoverageResult allPathsHitTraps(const Query &query) const;
  /// Context-insensitive structural slices. Use run() for context-sensitive
  /// source/sink witnesses. Reverse slicing never silently claims realizability.
  std::vector<FlowNodeID> slice(const std::vector<FlowNodeID> &seeds,
                              bool backward = false,
                              const std::vector<FlowNodeID> &traps = {}) const;
private:
  const FlowGraph &G;
};

} // namespace usehistory
} // namespace lotus
#endif
