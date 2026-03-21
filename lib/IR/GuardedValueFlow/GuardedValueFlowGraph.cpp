#include "IR/GuardedValueFlow/GuardedValueFlowGraph.h"

using namespace llvm;
using namespace llvm::gvg;

GuardedValueFlowGraph::GuardedValueFlowGraph(Function *base_function)
    : base_function_(base_function) {}

GuardedValueFlowNode *GuardedValueFlowGraph::findNode(Value *value) const {
  auto it = value_nodes_.find(value);
  return it == value_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapValueNode(Value *value,
                                         GuardedValueFlowNode *node) {
  if (value)
    value_nodes_[value] = node;
}

GuardedValueFlowCallSite *
GuardedValueFlowGraph::findCallSite(Instruction *inst) const {
  auto it = call_sites_.find(inst);
  return it == call_sites_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapCallSite(Instruction *inst,
                                        GuardedValueFlowCallSite *site) {
  if (inst)
    call_sites_[inst] = site;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findRegion(BasicBlock *block) const {
  auto it = regions_.find(block);
  return it == regions_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapRegion(BasicBlock *block,
                                      GuardedValueFlowRegionNode *node) {
  if (block)
    regions_[block] = node;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findLoadMemoryNode(Instruction *inst) const {
  auto it = load_memory_nodes_.find(inst);
  return it == load_memory_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapLoadMemoryNode(Instruction *inst,
                                              GuardedValueFlowNode *node) {
  if (inst)
    load_memory_nodes_[inst] = node;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findStoreMemoryNode(Value *value, Instruction *inst) const {
  auto it = store_memory_nodes_.find(std::make_pair(value, inst));
  return it == store_memory_nodes_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapStoreMemoryNode(Value *value, Instruction *inst,
                                               GuardedValueFlowNode *node) {
  store_memory_nodes_[std::make_pair(value, inst)] = node;
}
