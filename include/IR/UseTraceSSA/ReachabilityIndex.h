#ifndef LOTUS_IR_USETRACESSA_REACHABILITYINDEX_H
#define LOTUS_IR_USETRACESSA_REACHABILITYINDEX_H

#include "IR/UseTraceSSA/TraceFlowGraph.h"

namespace lotus {
namespace usetracessa {

/// SCC condensation plus lazily cached source-component closures. This index
/// answers CONTEXT-INSENSITIVE structural reachability, ignoring event traps.
/// A negative can prune a more restrictive query in the same graph abstraction;
/// a positive is NOT a realizable, trap-free, or path-feasible witness.
///
/// The graph must outlive the index. Mutation invalidates it and is diagnosed.
/// Each index is single-threaded; build one per querying thread or synchronize.
class ReachabilityIndex {
public:
  explicit ReachabilityIndex(const TraceFlowGraph &graph);
  bool mayReach(FlowNodeID source, FlowNodeID sink) const;
  bool mayReachAny(const std::vector<FlowNodeID> &sources,
                   const std::vector<FlowNodeID> &sinks) const;
  std::size_t components() const { checkRevision(); return DAG.size(); }
  std::size_t cachedSources() const { checkRevision(); return Cache.size(); }
  void clearCache() const { checkRevision(); Cache.clear(); }
private:
  void checkRevision() const;
  const TraceFlowGraph &G;
  std::uint64_t Revision;
  std::vector<ID> Component;
  std::vector<std::vector<ID>> DAG;
  mutable std::map<ID, std::vector<ID>> Cache;
};

} // namespace usetracessa
} // namespace lotus
#endif
