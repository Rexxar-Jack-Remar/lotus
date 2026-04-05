#include "Analysis/SymbolicExecution/SymbolicExecutionWrapper.h"

#include "Analysis/SymbolicExecution/SegUtility.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAdapter.h"

using namespace llvm;
using namespace SymbolicExecution;

#define DEBUG_TYPE "SymbolicExecutionWrapper"

char SymbolicExecutionWrapper::ID = 0;
static RegisterPass<SymbolicExecutionWrapper>
    X(DEBUG_TYPE, "Lotus symbolic execution wrapper");

SymbolicExecutionWrapper::SymbolicExecutionWrapper() : ModulePass(ID) {}
SymbolicExecutionWrapper::~SymbolicExecutionWrapper() = default;

void SymbolicExecutionWrapper::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<gsa::ControlDependenceAnalysisPass>();
  AU.addRequired<gsa::GateAnalysisPass>();
  AU.addRequired<lotus::gvfg::GuardedValueFlowGraphBuilderPass>();
  AU.addRequired<lotus::gvfg::LotusGuardedValueFlowAdapterPass>();
}

bool SymbolicExecutionWrapper::runOnModule(Module &M) {
  auto &builder = getAnalysis<lotus::gvfg::GuardedValueFlowGraphBuilderPass>();
  seg_utility::initAnalysisInterface(&M, &M.getDataLayout(), &builder);

  AnalysisDriver driver;
  driver.runOnModule(&M);
  return false;
}
