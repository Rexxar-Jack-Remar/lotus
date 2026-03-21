#include "IR/GuardedValueFlow/GuardedValueFlowGraph.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace llvm::gvg;

namespace {

static path_cond_t getImportedSource(path_cond_t cond) {
  if (!cond)
    return nullptr;
  if (cond->getKind() == PathCond::Kind::ImportedAtom)
    return cond->getImportedSource();
  return nullptr;
}

static Function *getInterfaceOriginFunction(path_cond_t cond) {
  if (!cond)
    return nullptr;

  if (path_cond_t imported = getImportedSource(cond))
    return imported->getOwnerFunc();

  if (cond->getKind() == PathCond::Kind::Not)
    return getInterfaceOriginFunction(cond->getLhs());

  if (cond->getKind() == PathCond::Kind::And || cond->getKind() == PathCond::Kind::Or) {
    Function *lhs_origin = getInterfaceOriginFunction(cond->getLhs());
    Function *rhs_origin = getInterfaceOriginFunction(cond->getRhs());
    return lhs_origin == rhs_origin ? lhs_origin : nullptr;
  }

  return nullptr;
}

static std::string renderPathCond(path_cond_t cond) {
  if (!cond)
    return "<null>";
  std::string buffer;
  raw_string_ostream os(buffer);
  cond->print(os);
  return os.str();
}

} // namespace

GuardedValueFlowGraph::GuardedValueFlowGraph(Function *base_function)
    : base_function_(base_function) {}

void GuardedValueFlowGraph::assignNodeRegion(GuardedValueFlowNode *node) {
  if (!node || node->getKind() == GuardedValueFlowNode::Kind::Region)
    return;

  BasicBlock *block = node->getParentBasicBlock();
  if (!block)
    return;

  GuardedValueFlowRegionNode *region = findRegion(block);
  if (!region && base_function_ && !base_function_->empty() &&
      block == &base_function_->getEntryBlock()) {
    region = getAlwaysTrueRegion();
  }

  if (region)
    node->region_ = region;
}

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

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findSemanticRegion(path_cond_t path_cond) const {
  auto it = semantic_regions_.find(path_cond);
  return it == semantic_regions_.end() ? nullptr : it->second;
}

GuardedValueFlowNode *
GuardedValueFlowGraph::findSemanticConditionNode(path_cond_t path_cond) const {
  auto it = semantic_condition_nodes_.find(path_cond);
  return it == semantic_condition_nodes_.end() ? nullptr : it->second;
}

GuardedValueFlowRegionNode *
GuardedValueFlowGraph::findOrCreateSemanticRegion(path_cond_t path_cond,
                                                  BasicBlock *block) {
  if (!path_cond)
    return getAlwaysTrueRegion();

  if (auto *existing = findSemanticRegion(path_cond))
    return existing;

  auto condition = ConditionRef::fromPathCond(path_cond);
  auto *condition_node = findSemanticConditionNode(path_cond);
  if (!condition_node) {
    condition_node = createNode<GuardedValueFlowNode>(
        GuardedValueFlowNode::Kind::InterfaceCondition,
        Type::getInt1Ty(base_function_->getContext()), this, nullptr, nullptr,
        nullptr);
    condition_node->setDescription("iface.cond:" + renderPathCond(path_cond));
    semantic_condition_nodes_[path_cond] = condition_node;
  }

  auto *region = createNode<GuardedValueFlowRegionNode>(
      Type::getInt1Ty(base_function_->getContext()), this, block,
      GuardedValueFlowRegionNode::Form::Interface, condition_node, true,
      condition);
  region->setDescription("region.interface");
  region->setInterfaceMetadata(path_cond->getOwnerFunc(),
                               getInterfaceOriginFunction(path_cond), path_cond,
                               getImportedSource(path_cond));
  region->addChild(condition_node, 1.0f, condition);
  condition_node->region_ = region;
  semantic_regions_[path_cond] = region;
  return region;
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

GuardedValueFlowNode *GuardedValueFlowGraph::findOrCreateStoreMemoryNode(
    Value *value, Instruction *inst, Type *type, BasicBlock *block,
    StringRef description) {
  if (auto *existing = findStoreMemoryNode(value, inst))
    return existing;

  auto *node = createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::StoreMemory, type, this, block, nullptr, inst);
  node->setDescription(description.str());
  mapStoreMemoryNode(value, inst, node);
  return node;
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

void GuardedValueFlowGraph::refreshNodeRegions() {
  for (const auto &node_ptr : nodes_)
    assignNodeRegion(node_ptr.get());
}
