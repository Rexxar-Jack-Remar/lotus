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
  StringRef getPassName() const override {
    return "LotusGuardedValueFlowAdapterPass";
  }

private:
  void adaptFunction(GuardedValueFlowGraph &graph, IntraLotusAA &pta);
};

ModulePass *createLotusGuardedValueFlowAdapterPass();

} // namespace gvg
} // namespace llvm
