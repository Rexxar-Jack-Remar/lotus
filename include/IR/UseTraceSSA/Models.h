#ifndef LOTUS_IR_USETRACESSA_MODELS_H
#define LOTUS_IR_USETRACESSA_MODELS_H

#include "IR/UseTraceSSA/Query.h"

namespace lotus {
namespace usetracessa {

struct ArgumentPorts {
  /// Value ports denote the operand AFTER its use at this call.
  std::vector<FlowNodeID> value;
  /// Memory ports must come from SVFG CallMu/CallChi or ActualIn/ActualOut,
  /// NOT the pointer address itself. They can contain multiple alias regions.
  std::vector<FlowNodeID> memoryIn;
  std::vector<FlowNodeID> memoryOut;
  /// Object-history after-use nodes; separate from byte-content value flow.
  std::vector<FlowNodeID> resourceUses;
  Certainty resourceCertainty = Certainty::May;
};
struct CallPorts {
  std::string callee;
  CallSiteID callSite = NoNativeID;
  std::vector<ArgumentPorts> arguments;
  std::vector<FlowNodeID> returnValue;
  std::vector<FlowNodeID> allocatedResources;
};

/// Explicit, replaceable library semantics. Pointee contents, address values,
/// and object lifetime state remain separate domains. Models append dependency
/// edges but do not silently remove SVFG edges or assume whole-buffer kills.
class LibraryModels {
public:
  using Model = std::function<void(TraceFlowGraph &, const CallPorts &)>;
  LibraryModels();
  void registerModel(std::string name, Model model);
  /// Unknown externals get a conservative input->output summary and a model
  /// issue. A negative query over an incomplete graph returns Unknown.
  void apply(TraceFlowGraph &graph, const CallPorts &call) const;
private:
  std::map<std::string, Model> Models;
};

namespace queries {
Query taint(const TraceFlowGraph &graph);
/// Includes release roots so a visible allocation is not required.
Query doubleFree(const TraceFlowGraph &graph);
Query useAfterFree(const TraceFlowGraph &graph);
/// Safety-check query: pass the relevant roots/dereferences. NonNull traps
/// must be attached to the successful CFG EDGE, never the comparison itself.
Query uncheckedUse(std::vector<FlowNodeID> sources, std::vector<FlowNodeID> uses);
} // namespace queries

} // namespace usetracessa
} // namespace lotus
#endif
