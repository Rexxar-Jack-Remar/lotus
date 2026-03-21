#include "IR/GuardedValueFlow/GuardedValueFlowNodes.h"

using namespace llvm;
using namespace llvm::gvg;

GuardedValueFlowNode::GuardedValueFlowNode(Kind kind, Type *type,
                                           GuardedValueFlowGraph *graph,
                                           BasicBlock *block, Value *llvm_value,
                                           Instruction *dbg_inst)
    : kind_(kind), type_(type), graph_(graph), block_(block),
      llvm_value_(llvm_value), dbg_inst_(dbg_inst) {}

void GuardedValueFlowNode::addChild(GuardedValueFlowNode *child,
                                    float confidence,
                                    ConditionRef condition) {
  children_.push_back({child, confidence, condition});
}

void GuardedValueFlowNode::addUseSite(GuardedValueFlowSite *site) {
  use_sites_.push_back(site);
}

void GuardedValueFlowNode::addMatchingCondition(GuardedValueFlowNode *producer,
                                                ConditionRef condition) {
  matching_conditions_.emplace(producer, condition);
}
