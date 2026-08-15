/// @file LotusAdapter.h
/// @brief Pass that populates a GuardedValueFlowGraph with LotusAA memory facts
///
/// The adapter replaces placeholder load/store memory edges, materialises
/// call-boundary pseudo-interface nodes, attaches access-path summary nodes,
/// imports cross-function path conditions, and records callee back-edge
/// metadata.  It is the bridge between the structural builder and the
/// interprocedural pointer analysis.

#pragma once

#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"

#include <llvm/Pass.h>

namespace lotus {
namespace gvfg {

using llvm::AnalysisUsage;
using llvm::IntraLotusAA;
using llvm::LotusAA;
using llvm::Module;
using llvm::ModulePass;
using llvm::StringRef;

/// ModulePass that adapts each function's GVFG with LotusAA results.
///
/// For every function:
///   - wires load-memory children to the store-memory nodes that LotusAA
///     determined are the reaching definitions
///   - creates pseudo-argument and pseudo-return nodes for each callee's
///     interface channels
///   - sets up per-callsite argument-summary and return-summary nodes
///   - imports cross-function path conditions into region nodes
///   - records back-edge information on callsites
///   - links output point-to results for pseudo outputs
class LotusGuardedValueFlowAdapterPass : public ModulePass {
public:
  static char ID;

  LotusGuardedValueFlowAdapterPass();

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;

  /// Dependency-preserving edge insertion. Inserts a legal cast when possible
  /// and an opaque coercion node plus diagnostic for incompatible/unknown
  /// types.
  static GuardedValueFlowNode *
  safeLink(GuardedValueFlowGraph &graph, GuardedValueFlowNode *parent,
           GuardedValueFlowNode *child, float confidence = 1.0f,
           ConditionRef condition = ConditionRef::none());
  StringRef getPassName() const override {
    return "LotusGuardedValueFlowAdapterPass";
  }

private:
  bool adaptFunction(GuardedValueFlowGraph &graph, IntraLotusAA &pta,
                     LotusAA &lotus, GuardedValueFlowGraphBuilderPass &builder);
};

ModulePass *createLotusGuardedValueFlowAdapterPass();

} // namespace gvfg
} // namespace lotus
