/*
 * Copyright 2026 Lotus contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "Analysis/Loop/LoopDependenceGraph.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

std::string describeValue(Value *value) {
  if (value == nullptr) {
    return "null";
  }

  if (auto *function = dyn_cast<Function>(value)) {
    return std::string("func:") + function->getName().str();
  }

  if (value->hasName()) {
    return value->getName().str();
  }

  std::string text;
  raw_string_ostream stream(text);
  value->printAsOperand(stream, false);
  return stream.str();
}

std::string describePDGNode(pdg::Node *node) {
  if (node == nullptr) {
    return "null-node";
  }

  auto *value = node->getValue();
  if (value != nullptr) {
    return describeValue(value);
  }

  return std::string("node-type:")
         + std::to_string(static_cast<int>(node->getNodeType()));
}

LoopDependenceEdgeKind classifyEdgeKind(pdg::EdgeType type) {
  switch (type) {
  case pdg::EdgeType::CONTROLDEP_CALLINV:
  case pdg::EdgeType::CONTROLDEP_CALLRET:
  case pdg::EdgeType::CONTROLDEP_ENTRY:
  case pdg::EdgeType::CONTROLDEP_BR:
  case pdg::EdgeType::CONTROLDEP_IND_BR:
    return LoopDependenceEdgeKind::Control;
  case pdg::EdgeType::DATA_DEF_USE:
  case pdg::EdgeType::VAL_DEP:
    return LoopDependenceEdgeKind::Variable;
  default:
    return LoopDependenceEdgeKind::Memory;
  }
}

LoopDependenceMemoryKind classifyMemoryKind(pdg::EdgeType type) {
  switch (type) {
  case pdg::EdgeType::DATA_RAW:
    return LoopDependenceMemoryKind::Raw;
  case pdg::EdgeType::DATA_READ:
    return LoopDependenceMemoryKind::ReadOnly;
  case pdg::EdgeType::DATA_ALIAS:
    return LoopDependenceMemoryKind::AliasUnknown;
  case pdg::EdgeType::DATA_DEF_USE:
  case pdg::EdgeType::VAL_DEP:
  case pdg::EdgeType::CONTROLDEP_CALLINV:
  case pdg::EdgeType::CONTROLDEP_CALLRET:
  case pdg::EdgeType::CONTROLDEP_ENTRY:
  case pdg::EdgeType::CONTROLDEP_BR:
  case pdg::EdgeType::CONTROLDEP_IND_BR:
    return LoopDependenceMemoryKind::None;
  default:
    return LoopDependenceMemoryKind::Unknown;
  }
}

bool isInternalToLoop(LoopStructure *loop, Value *value) {
  if (loop == nullptr || value == nullptr) {
    return false;
  }
  if (auto *instruction = dyn_cast<Instruction>(value)) {
    return loop->isIncluded(instruction);
  }
  return false;
}

bool shouldIgnoreLoopInstruction(Instruction *instruction) {
  if (instruction == nullptr) {
    return true;
  }
  if (isa<DbgInfoIntrinsic>(instruction)) {
    return true;
  }
  if (auto *call = dyn_cast<CallInst>(instruction)) {
    if (call->isLifetimeStartOrEnd()) {
      return true;
    }
  }
  return false;
}

bool shouldIgnoreValue(Value *value) {
  if (value == nullptr) {
    return true;
  }
  if (isa<BasicBlock>(value)) {
    return true;
  }
  if (isa<MetadataAsValue>(value)) {
    return true;
  }
  if (auto *inst = dyn_cast<Instruction>(value)) {
    return shouldIgnoreLoopInstruction(inst);
  }
  return false;
}

} // namespace

LoopDependenceNode::LoopDependenceNode(uint64_t id,
                                       Value *value,
                                       pdg::Node *pdgNode,
                                       bool internal)
    : id{id}, value{value}, pdgNode{pdgNode}, internal{internal} {}

uint64_t LoopDependenceNode::getID(void) const { return this->id; }

Value *LoopDependenceNode::getValue(void) const { return this->value; }

pdg::Node *LoopDependenceNode::getPDGNode(void) const { return this->pdgNode; }

bool LoopDependenceNode::isInternal(void) const { return this->internal; }

bool LoopDependenceNode::isExternal(void) const { return !this->internal; }

const std::vector<LoopDependenceEdge *> &
LoopDependenceNode::getIncomingEdges(void) const {
  return this->incomingEdges;
}

const std::vector<LoopDependenceEdge *> &
LoopDependenceNode::getOutgoingEdges(void) const {
  return this->outgoingEdges;
}

bool LoopDependenceNode::hasIncomingEdges(void) const {
  return !this->incomingEdges.empty();
}

bool LoopDependenceNode::hasOutgoingEdges(void) const {
  return !this->outgoingEdges.empty();
}

LoopDependenceEdge::LoopDependenceEdge(LoopDependenceNode *src,
                                       LoopDependenceNode *dst,
                                       LoopDependenceEdgeKind kind,
                                       LoopDependenceMemoryKind memoryKind,
                                       pdg::EdgeType originalEdgeType,
                                       bool loopCarried)
    : src{src},
      dst{dst},
      kind{kind},
      memoryKind{memoryKind},
      originalEdgeType{originalEdgeType},
      loopCarried{loopCarried} {}

LoopDependenceNode *LoopDependenceEdge::getSrc(void) const { return this->src; }

LoopDependenceNode *LoopDependenceEdge::getDst(void) const { return this->dst; }

LoopDependenceEdgeKind LoopDependenceEdge::getKind(void) const {
  return this->kind;
}

LoopDependenceMemoryKind LoopDependenceEdge::getMemoryKind(void) const {
  return this->memoryKind;
}

pdg::EdgeType LoopDependenceEdge::getOriginalEdgeType(void) const {
  return this->originalEdgeType;
}

bool LoopDependenceEdge::isLoopCarried(void) const {
  return this->loopCarried;
}

void LoopDependenceEdge::setLoopCarried(bool isLoopCarried) {
  this->loopCarried = isLoopCarried;
}

LoopDependenceGraph::LoopDependenceGraph(LoopTree *loopNode,
                                         pdg::ProgramGraph &pdg)
    : loop{loopNode}, pdg{&pdg} {
  assert(loopNode != nullptr);

  auto *loopStructure = this->getLoopStructure();
  assert(loopStructure != nullptr);

  std::vector<Instruction *> loopInstructions;
  for (auto *block : loopStructure->getBasicBlocks()) {
    for (auto &instruction : *block) {
      if (shouldIgnoreLoopInstruction(&instruction)) {
        continue;
      }
      loopInstructions.push_back(&instruction);
    }
  }

  std::sort(loopInstructions.begin(),
            loopInstructions.end(),
            [](Instruction *lhs, Instruction *rhs) {
              auto lhsBlock = lhs->getParent()->getName();
              auto rhsBlock = rhs->getParent()->getName();
              if (lhsBlock != rhsBlock) {
                return lhsBlock < rhsBlock;
              }
              return describeValue(lhs) < describeValue(rhs);
            });

  for (auto *instruction : loopInstructions) {
    auto *pdgNode = this->pdg->getNode(*instruction);
    this->fetchOrCreateNode(instruction, pdgNode, true);
  }

  std::vector<pdg::Edge *> pdgEdges;
  std::set<std::tuple<pdg::Node *, pdg::Node *, int>> seenEdges;
  std::queue<pdg::Node *> worklist;
  std::unordered_set<pdg::Node *> enqueued;
  for (auto *instruction : loopInstructions) {
    auto *pdgNode = this->pdg->getNode(*instruction);
    if (pdgNode != nullptr && enqueued.insert(pdgNode).second) {
      worklist.push(pdgNode);
    }
  }

  auto enqueueNode = [&](pdg::Node *node) {
    if (node == nullptr) {
      return;
    }
    auto *value = node->getValue();
    if (shouldIgnoreValue(value)) {
      return;
    }
    if (enqueued.insert(node).second) {
      worklist.push(node);
    }
  };

  while (!worklist.empty()) {
    auto *pdgNode = worklist.front();
    worklist.pop();

    for (auto *edge : pdgNode->getOutEdgeSet()) {
      auto key = std::make_tuple(edge->getSrcNode(),
                                 edge->getDstNode(),
                                 static_cast<int>(edge->getEdgeType()));
      if (seenEdges.insert(key).second) {
        pdgEdges.push_back(edge);
      }
      enqueueNode(edge->getDstNode());
      enqueueNode(edge->getSrcNode());
    }

    for (auto *edge : pdgNode->getInEdgeSet()) {
      auto key = std::make_tuple(edge->getSrcNode(),
                                 edge->getDstNode(),
                                 static_cast<int>(edge->getEdgeType()));
      if (seenEdges.insert(key).second) {
        pdgEdges.push_back(edge);
      }
      enqueueNode(edge->getSrcNode());
      enqueueNode(edge->getDstNode());
    }
  }

  std::sort(pdgEdges.begin(),
            pdgEdges.end(),
            [](pdg::Edge *lhs, pdg::Edge *rhs) {
              auto lhsSrc = describePDGNode(lhs->getSrcNode());
              auto rhsSrc = describePDGNode(rhs->getSrcNode());
              if (lhsSrc != rhsSrc) {
                return lhsSrc < rhsSrc;
              }
              auto lhsDst = describePDGNode(lhs->getDstNode());
              auto rhsDst = describePDGNode(rhs->getDstNode());
              if (lhsDst != rhsDst) {
                return lhsDst < rhsDst;
              }
              return static_cast<int>(lhs->getEdgeType())
                     < static_cast<int>(rhs->getEdgeType());
            });

  for (auto *edge : pdgEdges) {
    this->importEdge(edge);
  }

  std::set<std::tuple<Value *, Value *, int>> seenSynthetic;
  std::queue<Instruction *> syntheticWorklist;
  std::unordered_set<Instruction *> queuedSynthetic;
  for (auto *instruction : loopInstructions) {
    syntheticWorklist.push(instruction);
    queuedSynthetic.insert(instruction);
  }
  for (auto *node : this->getNodes()) {
    auto *instruction = dyn_cast_or_null<Instruction>(node->getValue());
    if (instruction != nullptr && queuedSynthetic.insert(instruction).second) {
      syntheticWorklist.push(instruction);
    }
  }

  while (!syntheticWorklist.empty()) {
    auto *instruction = syntheticWorklist.front();
    syntheticWorklist.pop();
    auto *dstNode = this->getNode(instruction);
    if (dstNode == nullptr) {
      auto *pdgNode = this->pdg->getNode(*instruction);
      dstNode = this->fetchOrCreateNode(
          instruction, pdgNode, isInternalToLoop(loopStructure, instruction));
    }
    for (auto &operand : instruction->operands()) {
      auto *value = operand.get();
      if (shouldIgnoreValue(value) || isa<Constant>(value)) {
        continue;
      }
      auto *srcNode = this->getNode(value);
      if (srcNode == nullptr) {
        auto *pdgNode = this->pdg->getNode(*value);
        srcNode = this->fetchOrCreateNode(
            value, pdgNode, isInternalToLoop(loopStructure, value));
      }
      auto key = std::make_tuple(value,
                                 static_cast<Value *>(instruction),
                                 static_cast<int>(pdg::EdgeType::DATA_DEF_USE));
      if (!seenSynthetic.insert(key).second) {
        continue;
      }
      auto ownedEdge = std::unique_ptr<LoopDependenceEdge>(new LoopDependenceEdge(
          srcNode,
          dstNode,
          LoopDependenceEdgeKind::Variable,
          LoopDependenceMemoryKind::None,
          pdg::EdgeType::DATA_DEF_USE,
          false));
      auto *rawEdge = ownedEdge.get();
      srcNode->outgoingEdges.push_back(rawEdge);
      dstNode->incomingEdges.push_back(rawEdge);
      this->ownedEdges.push_back(std::move(ownedEdge));

      if (auto *srcInst = dyn_cast<Instruction>(value)) {
        if (!shouldIgnoreLoopInstruction(srcInst)
            && queuedSynthetic.insert(srcInst).second) {
          syntheticWorklist.push(srcInst);
        }
      }
    }
  }
}

LoopTree *LoopDependenceGraph::getLoopHierarchyStructures(void) const {
  return this->loop;
}

LoopStructure *LoopDependenceGraph::getLoopStructure(void) const {
  return this->loop ? this->loop->getLoop() : nullptr;
}

std::vector<LoopDependenceNode *> LoopDependenceGraph::getInternalNodes(void) const {
  return this->internalNodes;
}

std::vector<LoopDependenceNode *> LoopDependenceGraph::getExternalNodes(void) const {
  return this->externalNodes;
}

std::vector<LoopDependenceNode *> LoopDependenceGraph::getNodes(void) const {
  std::vector<LoopDependenceNode *> nodes;
  nodes.reserve(this->ownedNodes.size());
  for (auto const &owned : this->ownedNodes) {
    nodes.push_back(owned.get());
  }
  return nodes;
}

std::vector<LoopDependenceEdge *> LoopDependenceGraph::getEdges(void) const {
  std::vector<LoopDependenceEdge *> edges;
  edges.reserve(this->ownedEdges.size());
  for (auto const &edge : this->ownedEdges) {
    edges.push_back(edge.get());
  }
  return edges;
}

std::vector<std::pair<Value *, LoopDependenceNode *>>
LoopDependenceGraph::internalNodePairs(void) const {
  std::vector<std::pair<Value *, LoopDependenceNode *>> pairs;
  pairs.reserve(this->internalNodes.size());
  for (auto *node : this->internalNodes) {
    pairs.emplace_back(node->getValue(), node);
  }
  return pairs;
}

std::vector<std::pair<Value *, LoopDependenceNode *>>
LoopDependenceGraph::externalNodePairs(void) const {
  std::vector<std::pair<Value *, LoopDependenceNode *>> pairs;
  pairs.reserve(this->externalNodes.size());
  for (auto *node : this->externalNodes) {
    pairs.emplace_back(node->getValue(), node);
  }
  return pairs;
}

LoopDependenceNode *LoopDependenceGraph::getNode(Value *value) const {
  auto it = this->nodesByValue.find(value);
  if (it == this->nodesByValue.end()) {
    return nullptr;
  }
  return it->second;
}

LoopDependenceNode *LoopDependenceGraph::fetchNode(Value *value) const {
  return this->getNode(value);
}

bool LoopDependenceGraph::doesItContain(Value *value) const {
  return this->getNode(value) != nullptr;
}

bool LoopDependenceGraph::isInternal(Value *value) const {
  auto *node = this->getNode(value);
  return node != nullptr && node->isInternal();
}

bool LoopDependenceGraph::isExternal(Value *value) const {
  auto *node = this->getNode(value);
  return node != nullptr && node->isExternal();
}

bool LoopDependenceGraph::iterateOverDependencesTo(
    Value *target,
    bool includeControl,
    bool includeVariable,
    bool includeMemory,
    const std::function<bool(Value *, LoopDependenceEdge *)> &funcToInvoke) const {
  auto *targetNode = this->getNode(target);
  if (targetNode == nullptr) {
    return false;
  }

  for (auto *edge : targetNode->getIncomingEdges()) {
    bool include = false;
    switch (edge->getKind()) {
    case LoopDependenceEdgeKind::Control:
      include = includeControl;
      break;
    case LoopDependenceEdgeKind::Variable:
      include = includeVariable;
      break;
    case LoopDependenceEdgeKind::Memory:
      include = includeMemory;
      break;
    }
    if (!include) {
      continue;
    }

    auto *src = edge->getSrc();
    if (src == nullptr) {
      continue;
    }
    if (funcToInvoke(src->getValue(), edge)) {
      return true;
    }
  }

  return false;
}

LoopDependenceNode *LoopDependenceGraph::fetchOrCreateNode(Value *value,
                                                           pdg::Node *pdgNode,
                                                           bool internal) {
  if (pdgNode != nullptr) {
    auto existingByPDG = this->nodesByPDGNode.find(pdgNode);
    if (existingByPDG != this->nodesByPDGNode.end()) {
      return existingByPDG->second;
    }
  }

  if (value != nullptr) {
    auto existingByValue = this->nodesByValue.find(value);
    if (existingByValue != this->nodesByValue.end()) {
      return existingByValue->second;
    }
  }

  auto node = std::unique_ptr<LoopDependenceNode>(new LoopDependenceNode(
      this->ownedNodes.size(), value, pdgNode, internal));
  auto *rawNode = node.get();
  this->ownedNodes.push_back(std::move(node));
  if (internal) {
    this->internalNodes.push_back(rawNode);
  } else {
    this->externalNodes.push_back(rawNode);
  }
  if (value != nullptr) {
    this->nodesByValue[value] = rawNode;
  }
  if (pdgNode != nullptr) {
    this->nodesByPDGNode[pdgNode] = rawNode;
  }
  return rawNode;
}

void LoopDependenceGraph::importEdge(pdg::Edge *edge) {
  auto *loopStructure = this->getLoopStructure();
  assert(loopStructure != nullptr);

  auto *srcPDGNode = edge->getSrcNode();
  auto *dstPDGNode = edge->getDstNode();
  Value *srcValue = srcPDGNode ? srcPDGNode->getValue() : nullptr;
  Value *dstValue = dstPDGNode ? dstPDGNode->getValue() : nullptr;
  if (shouldIgnoreValue(srcValue) || shouldIgnoreValue(dstValue)) {
    return;
  }

  auto srcInternal = isInternalToLoop(loopStructure, srcValue);
  auto dstInternal = isInternalToLoop(loopStructure, dstValue);
  if (!srcInternal && !dstInternal) {
    return;
  }

  auto *srcNode = this->fetchOrCreateNode(srcValue, srcPDGNode, srcInternal);
  auto *dstNode = this->fetchOrCreateNode(dstValue, dstPDGNode, dstInternal);
  auto edgeKind = classifyEdgeKind(edge->getEdgeType());
  auto memoryKind = classifyMemoryKind(edge->getEdgeType());

  auto ownedEdge = std::unique_ptr<LoopDependenceEdge>(new LoopDependenceEdge(
      srcNode, dstNode, edgeKind, memoryKind, edge->getEdgeType(), false));
  auto *rawEdge = ownedEdge.get();
  srcNode->outgoingEdges.push_back(rawEdge);
  dstNode->incomingEdges.push_back(rawEdge);
  this->ownedEdges.push_back(std::move(ownedEdge));
}

} // namespace loop
} // namespace analysis
} // namespace lotus
