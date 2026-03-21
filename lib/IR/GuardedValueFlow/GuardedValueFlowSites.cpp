#include "IR/GuardedValueFlow/GuardedValueFlowSites.h"

#include "IR/GuardedValueFlow/GuardedValueFlowGraph.h"

using namespace llvm;
using namespace llvm::gvg;

namespace {

static unsigned countPseudoNodes(const GuardedValueFlowGraph *graph,
                                 Instruction *call_site, Function *callee,
                                 GuardedValueFlowNode::Kind kind) {
  unsigned count = 0;
  if (!graph)
    return 0;

  for (const auto &node_ptr : graph->nodes()) {
    auto *call_node =
        dynamic_cast<GuardedValueFlowCallOutputNode *>(node_ptr.get());
    if (!call_node)
      continue;
    if (call_node->getKind() != kind)
      continue;
    if (call_node->getCallSite() != call_site)
      continue;
    if (call_node->getCallee() != callee)
      continue;
    ++count;
  }
  return count;
}

static GuardedValueFlowNode *
findPseudoNode(const GuardedValueFlowGraph *graph, Instruction *call_site,
               Function *callee, GuardedValueFlowNode::Kind kind,
               unsigned idx) {
  if (!graph)
    return nullptr;

  unsigned curr = 0;
  for (const auto &node_ptr : graph->nodes()) {
    auto *call_node =
        dynamic_cast<GuardedValueFlowCallOutputNode *>(node_ptr.get());
    if (!call_node)
      continue;
    if (call_node->getKind() != kind)
      continue;
    if (call_node->getCallSite() != call_site)
      continue;
    if (call_node->getCallee() != callee)
      continue;
    if (curr++ == idx)
      return call_node;
  }
  return nullptr;
}

} // namespace

void GuardedValueFlowCallSite::addCommonInput(GuardedValueFlowNode *node) {
  common_inputs_.push_back(node);
  if (node)
    node->addUseSite(this);
}

void GuardedValueFlowCallSite::addPseudoInput(Function *callee,
                                              GuardedValueFlowNode *node) {
  pseudo_inputs_[callee].push_back(node);
  if (node)
    node->addUseSite(this);
}

void GuardedValueFlowCallSite::addPseudoOutput(Function *callee,
                                               GuardedValueFlowNode *node) {
  pseudo_outputs_[callee].push_back(node);
}

void GuardedValueFlowCallSite::setCalleeCondition(
    Function *callee, ConditionRef condition, GuardedValueFlowRegionNode *region) {
  if (!callee)
    return;
  callee_conditions_[callee] = condition;
  if (region)
    callee_condition_regions_[callee] = region;
}

GuardedValueFlowNode *
GuardedValueFlowCallSite::getPseudoInput(Function *callee, unsigned idx) const {
  auto it = pseudo_inputs_.find(callee);
  if (it != pseudo_inputs_.end() && idx < it->second.size())
    return it->second[idx];
  return findPseudoNode(getGraph(), getInstruction(), callee,
                        GuardedValueFlowNode::Kind::CallSitePseudoInput, idx);
}

GuardedValueFlowNode *
GuardedValueFlowCallSite::getPseudoOutput(Function *callee,
                                          unsigned idx) const {
  auto it = pseudo_outputs_.find(callee);
  if (it != pseudo_outputs_.end() && idx < it->second.size())
    return it->second[idx];
  return findPseudoNode(getGraph(), getInstruction(), callee,
                        GuardedValueFlowNode::Kind::CallSitePseudoOutput, idx);
}

unsigned
GuardedValueFlowCallSite::getNumPseudoInputs(Function *callee) const {
  auto it = pseudo_inputs_.find(callee);
  if (it != pseudo_inputs_.end())
    return static_cast<unsigned>(it->second.size());
  return countPseudoNodes(getGraph(), getInstruction(), callee,
                          GuardedValueFlowNode::Kind::CallSitePseudoInput);
}

unsigned
GuardedValueFlowCallSite::getNumPseudoOutputs(Function *callee) const {
  auto it = pseudo_outputs_.find(callee);
  if (it != pseudo_outputs_.end())
    return static_cast<unsigned>(it->second.size());
  return countPseudoNodes(getGraph(), getInstruction(), callee,
                          GuardedValueFlowNode::Kind::CallSitePseudoOutput);
}
