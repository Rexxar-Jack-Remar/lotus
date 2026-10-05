#pragma once

#include "Checker/GSAF/Engine/Solver.h"
#include "Checker/GSAF/Support/MaskMap.h"
#include "Checker/GSAF/Support/ObjectOrder.h"
#include "IR/GVFG/GuardedValueFlowSerializer.h"
#include "Utils/LLVM/StringUtils.h"

#include <algorithm>
#include <sstream>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

namespace lotus {
namespace gsaf {

using gvfg::GuardedValueFlowArgumentNode;
using gvfg::GuardedValueFlowCallOutputNode;
using gvfg::GuardedValueFlowCallSite;
using gvfg::GuardedValueFlowDereferenceSite;
using gvfg::GuardedValueFlowDivSite;
using gvfg::GuardedValueFlowGEPReferenceSite;
using gvfg::GuardedValueFlowGraph;
using gvfg::GuardedValueFlowGraphBuilderPass;
using gvfg::GuardedValueFlowNode;
using gvfg::GuardedValueFlowObject;
using gvfg::GuardedValueFlowOpcodeNode;
using gvfg::GuardedValueFlowPhiNode;
using gvfg::GuardedValueFlowRegionNode;
using gvfg::GuardedValueFlowReturnNode;
using gvfg::GuardedValueFlowReturnSite;
using gvfg::GuardedValueFlowSite;
using gvfg::GuardedValueFlowSolver;

inline GuardedValueFlowCallSite *
callSite(const gvfg::GuardedValueFlowCallSummaryNode *node) {
  return node->getGraph()->findCallSite(node->getCallSite());
}

inline llvm::BasicBlock *immediateDominator(const llvm::DominatorTree &tree,
                                            llvm::BasicBlock *block) {
  auto *node = tree.getNode(block);
  return node && node->getIDom() ? node->getIDom()->getBlock() : nullptr;
}

inline bool hasContradictoryRegions(
    const std::vector<GuardedValueFlowRegionNode *> &regions,
    const GuardedValueFlowRegionNode *extra = nullptr) {
  std::map<const GuardedValueFlowNode *, bool> assignments;
  auto contradictory = [&](const GuardedValueFlowRegionNode *region) {
    if (!region)
      return false;
    if (!region->isSatisfiable())
      return true;
    for (const auto &entry : region->getConstraintState().assignments) {
      auto inserted = assignments.emplace(entry.first, entry.second);
      if (!inserted.second && inserted.first->second != entry.second)
        return true;
    }
    return false;
  };
  for (auto *region : regions)
    if (contradictory(region))
      return true;
  return contradictory(extra);
}

inline bool isOperand(const GuardedValueFlowObject *object) {
  return object && llvm::isa<GuardedValueFlowNode>(object) &&
         !llvm::isa<GuardedValueFlowOpcodeNode>(object);
}
inline const GuardedValueFlowNode *
operandNode(const GuardedValueFlowObject *object) {
  return isOperand(object) ? llvm::cast<GuardedValueFlowNode>(object) : nullptr;
}

inline const GuardedValueFlowSite *
operationalSite(const GuardedValueFlowSite *site) {
  using K = GuardedValueFlowSite::Kind;
  return site && site->getKind() != K::CallSite &&
                 site->getKind() != K::ReturnSite &&
                 site->getKind() != K::Unknown
             ? site
             : nullptr;
}

template <GuardedValueFlowNode::Kind Kind,
          typename NodeType = GuardedValueFlowNode>
const NodeType *nodeOfKind(const GuardedValueFlowObject *object) {
  auto *node = llvm::dyn_cast_or_null<GuardedValueFlowNode>(object);
  return node && node->getKind() == Kind ? llvm::dyn_cast<NodeType>(node)
                                         : nullptr;
}

inline GuardedValueFlowGraph *
graphFor(GuardedValueFlowGraphBuilderPass *builder, llvm::Function *function) {
  return function && builder->hasGraphFor(*function)
             ? &builder->getGraph(*function)
             : nullptr;
}

inline GuardedValueFlowCallSite *
callSite(const GuardedValueFlowCallOutputNode *node) {
  return node->getGraph()->findCallSite(node->getCallSite());
}

inline std::vector<GuardedValueFlowNode *>
valueFlowParents(const GuardedValueFlowNode *node, bool arithmetic) {
  auto result = node->getValueFlowParents(arithmetic);
  std::sort(result.begin(), result.end(), ObjectLess{});
  return result;
}

inline std::vector<GuardedValueFlowCallSite *>
callSites(const GuardedValueFlowGraph *graph) {
  std::vector<GuardedValueFlowCallSite *> result;
  for (const auto &site : graph->sites())
    if (auto *call = llvm::dyn_cast<GuardedValueFlowCallSite>(site.get()))
      result.push_back(call);
  return result;
}

inline std::vector<const GuardedValueFlowCallOutputNode *>
callOutputs(const GuardedValueFlowCallSite *site, llvm::Function *callee) {
  std::vector<const GuardedValueFlowCallOutputNode *> result;
  if (auto *output = site->getCommonOutput())
    result.push_back(llvm::cast<GuardedValueFlowCallOutputNode>(output));
  for (unsigned index = 0; index < site->getNumPseudoOutputs(callee); ++index)
    result.push_back(llvm::cast<GuardedValueFlowCallOutputNode>(
        site->getPseudoOutput(callee, index)));
  return result;
}

inline bool isGEPOpcode(const GuardedValueFlowNode *node) {
  auto *opcode = llvm::dyn_cast_or_null<GuardedValueFlowOpcodeNode>(node);
  return opcode && opcode->getOpcodeKind() ==
                       GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr;
}

inline llvm::Instruction *
gepMemoryUse(const GuardedValueFlowGEPReferenceSite *site) {
  auto *node = site->getGraph()->findNode(site->getInstruction());
  if (node)
    for (auto *use : node->useSites())
      if (llvm::isa<llvm::LoadInst>(use->getInstruction()) ||
          llvm::isa<llvm::StoreInst>(use->getInstruction()))
        return use->getInstruction();
  return nullptr;
}

inline std::vector<GuardedValueFlowNode *>
children(const GuardedValueFlowNode *node) {
  std::vector<GuardedValueFlowNode *> result;
  if (auto *opcode = llvm::dyn_cast<GuardedValueFlowOpcodeNode>(node)) {
    for (const auto &operand : opcode->operands())
      result.push_back(operand.producer);
  } else {
    for (const auto &edge : node->children())
      result.push_back(edge.target);
  }
  return result;
}

template <typename Tree>
std::vector<llvm::BasicBlock *> properDominators(const Tree *tree,
                                                 llvm::BasicBlock *block) {
  std::vector<llvm::BasicBlock *> result;
  auto *node = tree->getNode(block);
  if (!node)
    return result;
  for (node = node->getIDom(); node; node = node->getIDom())
    if (node->getBlock())
      result.push_back(node->getBlock());
  return result;
}

template <typename Tree>
std::vector<llvm::BasicBlock *>
properlyDominatedBlocks(const Tree *tree, llvm::BasicBlock *block) {
  std::vector<llvm::BasicBlock *> result;
  auto *node = tree->getNode(block);
  if (!node)
    return result;
  std::vector<llvm::DomTreeNode *> work;
  for (auto *child : *node)
    work.push_back(child);
  while (!work.empty()) {
    auto *current = work.back();
    work.pop_back();
    if (current->getBlock())
      result.push_back(current->getBlock());
    for (auto *child : *current)
      work.push_back(child);
  }
  return result;
}

inline std::string smtText(const SMTExprVec &values) {
  std::ostringstream stream;
  stream << "[";
  for (unsigned index = 0; index < values.size(); ++index) {
    if (index)
      stream << ", ";
    stream << values[index];
  }
  stream << "]";
  return stream.str();
}

template <typename T> std::string smtText(const T &value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

inline std::pair<SMTExprVec, SMTExprVec>
ctrlDepsPair(GuardedValueFlowSolver &solver, llvm::BasicBlock *block,
             const GuardedValueFlowGraph *graph,
             llvm::BasicBlock *previous_block) {
  GuardedValueFlowSolver::QueryContext context;
  context.previous_block = previous_block;
  return solver.getCtrlDepsPair(block, graph, &context);
}

} // namespace gsaf
} // namespace lotus
