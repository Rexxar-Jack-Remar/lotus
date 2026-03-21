#pragma once

#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "IR/GuardedValueFlow/GuardedValueFlowGraph.h"

#include <llvm/Pass.h>

namespace llvm {
namespace gvg {

class LotusGuardedValueFlowAdapterPass : public ModulePass {
public:
  static char ID;

  LotusGuardedValueFlowAdapterPass();

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;
  // This adapter turns the structural builder graph into the canonical
  // SEG-faithful GuardedValueFlowGraph by materializing LotusAA memory and
  // interprocedural interface semantics.
  static GuardedValueFlowNode *safeLink(GuardedValueFlowGraph &graph,
                                        GuardedValueFlowNode *parent,
                                        GuardedValueFlowNode *child,
                                        float confidence = 1.0f,
                                        ConditionRef condition =
                                            ConditionRef::none());
  StringRef getPassName() const override {
    return "LotusGuardedValueFlowAdapterPass";
  }

private:
  void adaptFunction(GuardedValueFlowGraph &graph, IntraLotusAA &pta,
                     LotusAA &lotus, GuardedValueFlowGraphBuilderPass &builder);
};

ModulePass *createLotusGuardedValueFlowAdapterPass();

} // namespace gvg
} // namespace llvm
