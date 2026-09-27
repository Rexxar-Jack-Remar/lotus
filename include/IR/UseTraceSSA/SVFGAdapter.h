#ifndef LOTUS_IR_USETRACESSA_SVFGADAPTER_H
#define LOTUS_IR_USETRACESSA_SVFGADAPTER_H

#include "IR/UseTraceSSA/TraceFlowGraph.h"

namespace lotus {
namespace usetracessa {

enum class PortKind { Definition, BeforeUse, AfterUse, Version, FlowNode };
struct Port {
  PortKind kind = PortKind::Definition;
  FunctionID function = InvalidID;
  ValueID value = InvalidID;
  SiteID site = InvalidID;
  VersionID version = InvalidID;
  FlowNodeID node = InvalidID;
  static Port definition(FunctionID f, ValueID v);
  static Port afterUse(FunctionID f, SiteID site, ValueID v);
  static Port flowNode(FlowNodeID node);
  FlowNodeID resolve(const TraceFlowGraph &graph) const;
};

/// A node output is a scalar SSA definition, a MemorySSA region/version, or
/// an actual/formal boundary port. Different memory versions MUST be distinct.
struct SVFGNodeRecord {
  NativeID id = NoNativeID;
  Port output;
};
struct SVFGEdgeRecord {
  NativeID id = NoNativeID;
  NativeID from = NoNativeID, to = NoNativeID;
  FlowKind kind = FlowKind::Direct;
  CallSiteID callSite = NoNativeID;
  ObjectSet objects = ObjectSet::unknown();
  std::uint64_t nativeKind = 0;
  std::string guard;
  /// For local def-use flow: the use of the SOURCE channel at the consumer.
  /// Must be AfterUse, so the raw SVFG edge cannot bypass earlier checks/uses.
  std::optional<Port> consumption;
  /// Only for true boundary ports (actual/formal, entry/exit, or summaries),
  /// whose output already denotes state at the transfer point.
  bool sourceIsBoundary = false;
};
struct SVFGSnapshot {
  std::vector<SVFGNodeRecord> nodes;
  std::vector<SVFGEdgeRecord> edges;
  std::vector<std::string> issues;
};
struct SVFGImportResult {
  std::map<NativeID, FlowNodeID> nodes;
  std::map<NativeID, FlowEdgeID> edges;
};

class SVFGAdapter {
public:
  /// Strict, transactional import. Invalid/missing use bindings throw; the
  /// supplied graph is unchanged. No pointer analysis is run or replaced.
  static SVFGImportResult append(TraceFlowGraph &graph, const SVFGSnapshot &snapshot);

  /// Generic SVF-style graph iterator bridge. Node/edge mappers
  /// must retain actual edge kinds, call-site IDs and points-to sets. They also
  /// bind scalar operands / MemorySSA versions to history ports. This explicit
  /// seam avoids depending on unstable subclasses or guessing memory semantics.
  ///
  ///   auto imported = SVFGAdapter::appendNative(graph, *svfg, nodeMapper, edgeMapper);
  ///
  /// Use snapshot() below for upstream graphs with different iterator APIs.
  template <typename NativeGraph, typename NodeMapper, typename EdgeMapper>
  static SVFGImportResult appendNative(TraceFlowGraph &graph, const NativeGraph &native,
                                      NodeMapper nodeMapper, EdgeMapper edgeMapper) {
    SVFGSnapshot view;
    for (const auto &item : native) {
      const auto &node = *item.second;
      view.nodes.push_back(nodeMapper(node));
      for (auto it = node.OutEdgeBegin(); it != node.OutEdgeEnd(); ++it)
        view.edges.push_back(edgeMapper(**it));
    }
    return append(graph, view);
  }
};

} // namespace usetracessa
} // namespace lotus
#endif
