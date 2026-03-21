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

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findUnitRegion(GuardedValueFlowNode *condition,
                                      bool sense) const {
  auto it = unit_regions_.find(std::make_pair(condition, sense));
  return it == unit_regions_.end() ? nullptr : it->second;
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::findOrCreateUnitRegion(
    GuardedValueFlowNode *condition, bool sense, BasicBlock *block,
    ConditionRef condition_ref) {
  if (!condition && sense)
    return getAlwaysTrueRegion();
  if (!condition && !sense)
    return getAlwaysFalseRegion();

  if (auto *existing = findUnitRegion(condition, sense))
    return existing;

  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      GuardedValueFlowRegionNode::Form::Unit, condition, sense, condition_ref);
  if (condition)
    region->addChild(condition, 1.0f, condition_ref);
  unit_regions_[std::make_pair(condition, sense)] = region;
  return region;
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::findOrCreateAndRegion(
    GuardedValueFlowRegionNode *lhs, GuardedValueFlowRegionNode *rhs,
    BasicBlock *block) {
  if (!lhs)
    return rhs ? rhs : getAlwaysTrueRegion();
  if (!rhs)
    return lhs;
  if (lhs == rhs)
    return lhs;
  if (lhs->isAlwaysTrue())
    return rhs;
  if (rhs->isAlwaysTrue())
    return lhs;
  if (lhs->isAlwaysFalse())
    return lhs;
  if (rhs->isAlwaysFalse())
    return rhs;

  auto key = std::minmax(lhs, rhs);
  auto it = and_regions_.find(key);
  if (it != and_regions_.end())
    return it->second;

  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      GuardedValueFlowRegionNode::Form::And, nullptr, true,
      ConditionRef::none());
  region->addChild(lhs);
  region->addChild(rhs);
  and_regions_[key] = region;
  return region;
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::findOrCreateOrRegion(
    GuardedValueFlowRegionNode *lhs, GuardedValueFlowRegionNode *rhs,
    BasicBlock *block) {
  if (!lhs)
    return rhs ? rhs : getAlwaysTrueRegion();
  if (!rhs)
    return lhs;
  if (lhs == rhs)
    return lhs;
  if (lhs->isAlwaysTrue())
    return lhs;
  if (rhs->isAlwaysTrue())
    return rhs;
  if (lhs->isAlwaysFalse())
    return rhs;
  if (rhs->isAlwaysFalse())
    return lhs;

  auto key = std::minmax(lhs, rhs);
  auto it = or_regions_.find(key);
  if (it != or_regions_.end())
    return it->second;

  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      GuardedValueFlowRegionNode::Form::Or, nullptr, true,
      ConditionRef::none());
  region->addChild(lhs);
  region->addChild(rhs);
  or_regions_[key] = region;
  return region;
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::findOrCreateNotRegion(
    GuardedValueFlowRegionNode *input, BasicBlock *block) {
  if (!input)
    return getAlwaysFalseRegion();
  if (input->isAlwaysTrue())
    return getAlwaysFalseRegion();
  if (input->isAlwaysFalse())
    return getAlwaysTrueRegion();
  if (input->getForm() == GuardedValueFlowRegionNode::Form::Unit)
    return findOrCreateUnitRegion(input->getConditionNode(),
                                  !input->getConditionSense(), block,
                                  input->getRegionCondition());

  auto it = not_regions_.find(input);
  if (it != not_regions_.end())
    return it->second;

  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      GuardedValueFlowRegionNode::Form::Not, nullptr, true,
      ConditionRef::none());
  region->addChild(input);
  not_regions_[input] = region;
  return region;
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::getAlwaysTrueRegion() {
  if (!always_true_region_) {
    always_true_region_ = createNode<GuardedValueFlowRegionNode>(
        Type::getInt1Ty(base_function_->getContext()), this, nullptr,
        GuardedValueFlowRegionNode::Form::AlwaysTrue, nullptr, true,
        ConditionRef::none());
  }
  return always_true_region_;
}

GuardedValueFlowRegionNode *GuardedValueFlowGraph::getAlwaysFalseRegion() {
  if (!always_false_region_) {
    always_false_region_ = createNode<GuardedValueFlowRegionNode>(
        Type::getInt1Ty(base_function_->getContext()), this, nullptr,
        GuardedValueFlowRegionNode::Form::AlwaysFalse, nullptr, false,
        ConditionRef::none());
  }
  return always_false_region_;
}

void GuardedValueFlowGraph::addBlockCondition(BasicBlock *block,
                                              BlockCondition condition) {
  if (!block)
    return;
  block_conditions_[block].push_back(condition);
}

ArrayRef<GuardedValueFlowGraph::BlockCondition>
GuardedValueFlowGraph::getBlockConditions(BasicBlock *block) const {
  auto it = block_conditions_.find(block);
  if (it == block_conditions_.end())
    return {};
  return it->second;
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

GuardedValueFlowReturnSite *
GuardedValueFlowGraph::findReturnSite(Instruction *inst) const {
  auto it = return_sites_.find(inst);
  return it == return_sites_.end() ? nullptr : it->second;
}

void GuardedValueFlowGraph::mapReturnSite(Instruction *inst,
                                          GuardedValueFlowReturnSite *site) {
  if (inst)
    return_sites_[inst] = site;
}
