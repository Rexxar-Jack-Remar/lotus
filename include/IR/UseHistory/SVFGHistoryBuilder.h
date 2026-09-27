#ifndef LOTUS_IR_USEHISTORY_SVFGHISTORYBUILDER_H
#define LOTUS_IR_USEHISTORY_SVFGHISTORYBUILDER_H

#include "IR/UseHistory/SVFGAdapter.h"

namespace lotus {
namespace usehistory {

struct FunctionLayout {
  FunctionID id = InvalidID;
  std::string name;
  /// Only topology and operation order are used. An operation can be on a
  /// block or a CFG edge. Give distinct semantic phases distinct sites.
  Program control;
};
struct LocatedSVFGNode {
  NativeID id = NoNativeID;
  FunctionID function = InvalidID;
  /// InvalidID denotes live-on-entry (formal parameters / memory inputs).
  SiteID definitionSite = InvalidID;
  std::string label;
  Event definitionEvents = Event::None;
  Certainty certainty = Certainty::Must;
};
struct LocatedSVFGEdge {
  NativeID id = NoNativeID, from = NoNativeID, to = NoNativeID;
  FlowKind kind = FlowKind::Direct;
  CallSiteID callSite = NoNativeID;
  ObjectSet objects;
  std::uint64_t nativeKind = 0;
  /// Local uses default to the target definition's site. LLVM PHI / MemorySSA
  /// phi operands MUST explicitly name their incoming-edge site here.
  SiteID consumerSite = InvalidID;
  /// Only actual/formal, entry/exit or explicit summary ports may omit a local
  /// use. Interprocedural transfers must opt in and retain call-site metadata.
  bool boundary = false;
  Event useEvents = Event::None;
  Certainty certainty = Certainty::Must;
  std::string guard;
};
struct SVFGConstructionInput {
  std::vector<FunctionLayout> functions;
  std::vector<LocatedSVFGNode> nodes;
  std::vector<LocatedSVFGEdge> edges;
  std::vector<std::string> issues;
};
struct SVFGHistoryResult {
  FlowGraph graph;
  SVFGImportResult native;
  std::map<NativeID, ValueID> channels;
  std::map<FunctionID, std::map<SiteID, SiteID>> sites;
};

/// Builds history SSA for the existing SVFG's scalar and MemorySSA-version
/// channels, then REPLACES each local raw def-use transfer by an after-use
/// transfer. Input contains no alias solver and no recomputed points-to sets.
///
/// This normalized API is deliberately independent of LOTUS SVFG subclasses.
/// A native frontend supplies instruction/edge locations and copies upstream
/// edge metadata; there is no safe way to recover use order from adjacency alone.
class SVFGHistoryBuilder {
public:
  static SVFGHistoryResult build(const SVFGConstructionInput &input);
};

} // namespace usehistory
} // namespace lotus
#endif
