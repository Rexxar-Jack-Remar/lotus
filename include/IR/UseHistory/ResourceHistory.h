#ifndef LOTUS_IR_USEHISTORY_RESOURCEHISTORY_H
#define LOTUS_IR_USEHISTORY_RESOURCEHISTORY_H

#include "IR/UseHistory/FlowGraph.h"

namespace lotus {
namespace usehistory {

/// Reserved only within the resource-lane domain. Unknown accesses are also
/// expanded to every known object in the supplied module-wide universe.
constexpr ObjectID UnknownResource = std::numeric_limits<ObjectID>::max();
struct ResourceAccess {
  SiteID site = InvalidID;
  ObjectSet objects;
  Event events = Event::None;
  /// Do not infer Must from a singleton may-points-to set: null, summary
  /// objects, conditional effects and partial writes still matter.
  Certainty certainty = Certainty::May;
};
struct ResourceLayer {
  FunctionID id = InvalidID;
  std::map<ObjectID, ValueID> values;
  std::map<SiteID, SiteID> sites;
  std::map<BlockID, SiteID> exits;
  FlowNodeID before(const FlowGraph &g, SiteID originalSite, ObjectID object) const;
  FlowNodeID after(const FlowGraph &g, SiteID originalSite, ObjectID object) const;
  FlowNodeID entry(const FlowGraph &g, ObjectID object) const;
};

class ResourceHistoryBuilder {
public:
  /// Reuse upstream points-to sets. No may-alias union-find. Histories are
  /// built per abstract object over the actual CFG, preserving branches/loops.
  /// The control Program supplies ONLY topology/site ordering; its values and
  /// definitions are ignored. Edge sites and parallel CFG edges are retained.
  ///
  /// Supply the SAME module-wide universe to all communicating functions.
  /// A top/unknown access also creates UnknownResource. Empty sets touch none.
  static ResourceLayer append(FlowGraph &graph, FunctionID layer, std::string name,
                              const Program &control,
                              const std::vector<ResourceAccess> &accesses,
                              const std::vector<ObjectID> &objectUniverse = {});

  /// Splice one call's object histories through ALL resolved targets. At the
  /// caller site, add a None-event ResourceAccess covering the universe first.
  /// Complete calls replace the before->after bypass; partial/unknown target
  /// sets retain it and mark the model incomplete. Call-site symbols match
  /// returns, including recursion and multiple callers of one callee.
  ///
  /// Targets must expose all relevant normal exits. Model exceptional exits
  /// as separate edge-site calls/ports rather than conflating unwind and normal.
  static void connectCall(FlowGraph &graph, const ResourceLayer &caller,
                          SiteID site, CallSiteID callSite,
                          const std::vector<ResourceLayer> &targets,
                          bool completeTargets = true);
};

} // namespace usehistory
} // namespace lotus
#endif
