#include "IR/GuardedValueFlow/GuardedValueFlowNodes.h"
#include "IR/GuardedValueFlow/GuardedValueFlowSites.h"

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
  for (GuardedValueFlowSite *existing : use_sites_) {
    if (existing == site)
      return;
  }
  use_sites_.push_back(site);
}

void GuardedValueFlowNode::addMatchingRegion(GuardedValueFlowNode *producer,
                                             GuardedValueFlowRegionNode *region,
                                             ConditionRef provenance) {
  for (auto &existing : matching_regions_) {
    if (existing.producer == producer) {
      existing.region = region;
      existing.provenance = provenance;
      return;
    }
  }
  matching_regions_.push_back({producer, region, provenance});
}

GuardedValueFlowRegionNode *
GuardedValueFlowNode::getMatchingRegion(
    const GuardedValueFlowNode *producer) const {
  for (const auto &entry : matching_regions_) {
    if (entry.producer == producer)
      return entry.region;
  }
  return nullptr;
}

ConditionRef GuardedValueFlowNode::getMatchingCondition(
    const GuardedValueFlowNode *producer) const {
  for (const auto &entry : matching_regions_) {
    if (entry.producer == producer)
      return entry.provenance;
  }
  return ConditionRef::none();
}

void GuardedValueFlowPhiNode::addIncoming(GuardedValueFlowNode *value_node,
                                          BasicBlock *incoming_block,
                                          GuardedValueFlowNode *condition_node,
                                          bool condition_sense,
                                          ConditionRef condition) {
  incoming_.push_back(
      {value_node, incoming_block, condition_node, condition_sense, condition});
  addChild(value_node, 1.0f, condition);
}

void GuardedValueFlowReturnNode::addReturnValueSitePair(
    GuardedValueFlowNode *value_node, GuardedValueFlowReturnSite *site) {
  return_sites_[value_node] = site;
}

GuardedValueFlowReturnSite *
GuardedValueFlowReturnNode::getReturnSite(
    const GuardedValueFlowNode *value_node) const {
  auto it = return_sites_.find(value_node);
  return it == return_sites_.end() ? nullptr : it->second;
}
