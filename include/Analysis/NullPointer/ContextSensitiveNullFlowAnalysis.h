/*
 *  Author: rainoftime
 *  Date: 2025-03
 *  Description: Context-sensitive null flow analysis
 */

#ifndef NULLPOINTER_CONTEXTSENSITIVENULLFLOWANALYSIS_H
#define NULLPOINTER_CONTEXTSENSITIVENULLFLOWANALYSIS_H

#include <set>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
// #include <map>
#include "Alias/DyckAA/DyckVFG.h"
#include "Analysis/NullPointer/AliasAnalysisAdapter.h"

#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;

class DyckAliasAnalysis;

// Context sensitive context
typedef std::vector<CallInst *> Context;

// Function context pair
typedef std::pair<Function *, Context> FunctionContextPair;

// Hash function for FunctionContextPair
namespace std {
template <> struct hash<FunctionContextPair> {
  size_t operator()(const FunctionContextPair &FCP) const {
    size_t H = hash<Function *>()(FCP.first);
    for (auto *CI : FCP.second) {
      H += hash<CallInst *>()(CI);
    }
    return H;
  }
};

template <> struct equal_to<FunctionContextPair> {
  bool operator()(const FunctionContextPair &LHS,
                  const FunctionContextPair &RHS) const {
    if (LHS.first != RHS.first)
      return false;
    if (LHS.second.size() != RHS.second.size())
      return false;
    for (unsigned K = 0; K < LHS.second.size(); ++K) {
      if (LHS.second[K] != RHS.second[K])
        return false;
    }
    return true;
  }
};
} // namespace std

// VFG edge used in non-null propagation
typedef std::pair<DyckVFGNode *, DyckVFGNode *> VFGEdge;

// mapping from context to sets of non-null edges/nodes
typedef std::unordered_map<FunctionContextPair, std::set<VFGEdge>>
    NonNullEdgesMap;
typedef std::unordered_map<FunctionContextPair, std::set<DyckVFGNode *>>
    NonNullNodesMap;
typedef NonNullEdgesMap NewNonNullEdgesMap;

class ContextSensitiveNullFlowAnalysis : public ModulePass {
private:
  // Alias analysis adapter - uses DyckAA
  AliasAnalysisAdapter *AAA;

  // Dyck alias analysis (for call graph)
  DyckAliasAnalysis *DAA;

  // VFG from DyckValueFlowAnalysis
  DyckVFG *VFG;

  // Max context depth
  unsigned MaxContextDepth;

  // NonNull edges collected during the analysis for each function & context
  NewNonNullEdgesMap NewNonNullEdges;

  // Known non-null edges/nodes per function & context
  NonNullEdgesMap NonNullEdges;
  NonNullNodesMap NonNullNodes;

  // Internally created alias analysis adapter - needs to be deleted
  bool OwnsAliasAnalysisAdapter;

public:
  static char ID;

  ContextSensitiveNullFlowAnalysis();

  ~ContextSensitiveNullFlowAnalysis() override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnModule(Module &M) override;

  // return true if Ptr can not be a null pointer
  bool notNull(Value *Ptr, Context Ctx) const;

  void add(Function *F, Context Ctx, Value *V1, Value *V2 = nullptr);

  void add(Function *F, Context Ctx, CallInst *CI, unsigned int K);

  void add(Function *F, Context Ctx, Value *Ret);

  // Helper method to get a context string for debugging
  std::string getContextString(const Context &Ctx) const;

  // Helper method to create a new context by extending an existing one
  Context extendContext(const Context &Ctx, CallInst *CI) const;

  // Recompute analysis with new non-null edges
  bool recompute(
      std::set<std::pair<Function *, Context>> &NewNonNullFunctionContexts);
};

#endif // NULLPOINTER_CONTEXTSENSITIVENULLFLOWANALYSIS_H
