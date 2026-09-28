#pragma once

#include "IR/UseTraceSSA/TraceFlowGraph.h"

namespace lotus {
namespace usetracessa {

struct TemporalEffect {
  SiteID site = InvalidID;
  ObjectSet objects;
  Event events = Event::None;
  Certainty certainty = Certainty::May;
  NativeID native = NoNativeID;
};

/// One execution history per CFG, independent of abstract objects. Uses at a
/// site share one psi; branches/loops share history phis. The execution token
/// denotes control position, never an abstract object or an alias class.
struct TemporalHistory {
  FunctionID id = InvalidID;
  ValueID execution = InvalidID;
  std::map<SiteID, SiteID> sites;
  std::map<BlockID, SiteID> exits;
  FlowNodeID before(const TraceFlowGraph &g, SiteID site) const;
  FlowNodeID after(const TraceFlowGraph &g, SiteID site) const;
  FlowNodeID entry(const TraceFlowGraph &g) const;

  // OLD build cost multiplied temporal structure by object multiplicity.
  // Here construction visits sites once. Object costs are metadata storage
  // and query-time contains()/mask intersections, never temporal nodes.
  static TemporalHistory append(TraceFlowGraph &graph, FunctionID id,
                                std::string name, const Program &control,
                                const std::vector<TemporalEffect> &effects);
  /// Splice normal temporal ports with the ordinary typed call/return edges.
  /// Each target gets one call and one return per exit, regardless of objects.
  static void connectCall(TraceFlowGraph &graph, const TemporalHistory &caller,
                          SiteID site, CallSiteID callSite,
                          const std::vector<TemporalHistory> &targets,
                          bool completeTargets = true,
                          ObjectSet objects = ObjectSet::unknown());
};

} // namespace usetracessa
} // namespace lotus
