/// @file ICFG.cpp
/// @brief Implementation of ICFG node and edge operations.

#include "IR/ICFG/ICFG.h"

#include <iostream>

#include <llvm/IR/Instructions.h>

using namespace llvm;

//
//=============================================================================
// ICFG Node
//=============================================================================
//

std::string ICFGNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "ICFGNode ID: " << getId();
  return rawstr.str();
}

void ICFGNode::dump() const {

  std::cout << this->toString() << "\n";
  std::cout << "OutEdges:\n";
  for (auto *edge : getOutEdges()) {

    std::cout << "\t" << edge->toString() << "\n";
  }
  std::cout << "InEdges:\n";
  for (auto *edge : getInEdges()) {

    std::cout << "\t" << edge->toString() << "\n";
  }
}

std::string IntraBlockNode::toString() const {

  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "IntraBlockNode ID: " << getId();
  if (getBasicBlock()->hasName()) {

    rawstr << ", Name: " << getBasicBlock()->getName().str();
  }

  return rawstr.str();
}

std::string FunEntryBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "FunEntryBlockNode ID: " << getId();
  if (getFunction())
    rawstr << ", Function: " << getFunction()->getName();
  return rawstr.str();
}

std::string FunExitBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "FunExitBlockNode ID: " << getId();
  if (getFunction())
    rawstr << ", Function: " << getFunction()->getName();
  return rawstr.str();
}

std::string CallRetBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "CallRetBlockNode ID: " << getId();
  if (getFunction())
    rawstr << ", Function: " << getFunction()->getName();
  if (callSite)
    rawstr << ", CallSite: " << *callSite;
  return rawstr.str();
}

//
//=============================================================================
// ICFG Edge
//=============================================================================
//

std::string ICFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "ICFGEdge: [" << getDstID() << "<--" << getSrcID() << "]\t";
  return rawstr.str();
}

std::string IntraCFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "IntraCFGEdge: [" << getDstID() << "<--" << getSrcID() << "]\t";

  return rawstr.str();
}

std::string CallCFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "CallCFGEdge " << " [";
  rawstr << getDstID() << "<--" << getSrcID() << "]\t CallSite: " << *cs
         << "\t";
  return rawstr.str();
}

std::string RetCFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "RetCFGEdge " << " [";
  rawstr << getDstID() << "<--" << getSrcID() << "]\t CallSite: " << *cs
         << "\t";
  return rawstr.str();
}

//
//=============================================================================
// ICFG
//=============================================================================
//

/// @brief Constructs an empty ICFG.
ICFG::ICFG() : totalICFGNode(0) {}

/// @brief Checks if an intraprocedural edge exists between two nodes.
ICFGEdge *ICFG::hasIntraICFGEdge(ICFGNode *src, ICFGNode *dst,
                                 ICFGEdge::ICFGEdgeK kind) {
  ICFGEdge edge(src, dst, kind);
  ICFGEdge *outEdge = src->hasOutgoingEdge(&edge);
  ICFGEdge *inEdge = dst->hasIncomingEdge(&edge);
  if (outEdge && inEdge) {
    assert(outEdge == inEdge && "edges not match");
    return outEdge;
  }
  return nullptr;
}

/// @brief Checks if an interprocedural edge exists between two nodes.
ICFGEdge *ICFG::hasInterICFGEdge(ICFGNode *src, ICFGNode *dst,
                                 ICFGEdge::ICFGEdgeK kind) {
  ICFGEdge edge(src, dst, kind);
  ICFGEdge *outEdge = src->hasOutgoingEdge(&edge);
  ICFGEdge *inEdge = dst->hasIncomingEdge(&edge);
  if (outEdge && inEdge) {
    assert(outEdge == inEdge && "edges not match");
    return outEdge;
  }
  return nullptr;
}

/// @brief Retrieves an edge between two nodes of a specific kind.
ICFGEdge *ICFG::getICFGEdge(const ICFGNode *src, const ICFGNode *dst,
                            ICFGEdge::ICFGEdgeK kind) {
  ICFGEdge *edge = nullptr;
  size_t counter = 0;
  for (auto iter = src->OutEdgeBegin(); iter != src->OutEdgeEnd(); ++iter) {
    if ((*iter)->getDstID() == dst->getId() && (*iter)->getEdgeKind() == kind) {
      counter++;
      edge = (*iter);
    }
  }
  assert(counter <= 1 && "there's more than one edge between two ICFG nodes");
  return edge;
}

/// @brief Adds an intraprocedural edge between two nodes.
ICFGEdge *ICFG::addIntraEdge(ICFGNode *srcNode, ICFGNode *dstNode) {
  checkIntraEdgeParents(srcNode, dstNode);
  if (hasIntraICFGEdge(srcNode, dstNode, ICFGEdge::IntraCF))
    return nullptr;
  IntraCFGEdge *intraEdge = new IntraCFGEdge(srcNode, dstNode);
  return addICFGEdge(intraEdge) ? intraEdge : nullptr;
}

/// @brief Adds an interprocedural call edge from caller to callee.
ICFGEdge *ICFG::addCallEdge(ICFGNode *srcNode, ICFGNode *dstNode,
                            const llvm::Instruction *cs) {
  if (hasInterICFGEdge(srcNode, dstNode, ICFGEdge::CallCF))
    return nullptr;
  CallCFGEdge *callEdge = new CallCFGEdge(srcNode, dstNode, cs);
  return addICFGEdge(callEdge) ? callEdge : nullptr;
}

/// @brief Adds an interprocedural return edge from callee to caller.
ICFGEdge *ICFG::addRetEdge(ICFGNode *srcNode, ICFGNode *dstNode,
                           const llvm::Instruction *cs) {
  if (hasInterICFGEdge(srcNode, dstNode, ICFGEdge::RetCF))
    return nullptr;
  RetCFGEdge *retEdge = new RetCFGEdge(srcNode, dstNode, cs);
  return addICFGEdge(retEdge) ? retEdge : nullptr;
}

bool ICFG::hasIntraBlockNode(const llvm::BasicBlock *bb) {

  IntraBlockNode *node = getIntraBlockICFGNode(bb);
  return node != nullptr;
}

IntraBlockNode *ICFG::getIntraBlockNode(const llvm::BasicBlock *bb) {

  IntraBlockNode *node = getIntraBlockICFGNode(bb);
  if (node == nullptr)
    node = addIntraBlockICFGNode(bb);
  return node;
}

FunEntryBlockNode *ICFG::addFunEntryICFGNode(const llvm::Function *F) {
  if (!F || F->isDeclaration())
    return nullptr;
  const BasicBlock *bb = &F->getEntryBlock();
  auto *node = new FunEntryBlockNode(totalICFGNode++, bb);
  addICFGNode(node);
  functionToEntryNodeMap[F] = node;
  return node;
}

FunExitBlockNode *ICFG::addFunExitICFGNode(const llvm::Function *F) {
  if (!F || F->isDeclaration())
    return nullptr;
  const BasicBlock *anchor = &F->getEntryBlock();
  for (const BasicBlock &bb : *F) {
    if (isa<ReturnInst>(bb.getTerminator())) {
      anchor = &bb;
      break;
    }
  }
  auto *node = new FunExitBlockNode(totalICFGNode++, anchor);
  addICFGNode(node);
  functionToExitNodeMap[F] = node;
  return node;
}

CallRetBlockNode *ICFG::addRetICFGNode(const llvm::Instruction *callInst) {
  if (!callInst)
    return nullptr;
  const BasicBlock *retBB = callInst->getParent();
  if (const auto *invokeInst = dyn_cast<InvokeInst>(callInst))
    retBB = invokeInst->getNormalDest();
  auto *node = new CallRetBlockNode(totalICFGNode++, callInst, retBB);
  addICFGNode(node);
  callToRetNodeMap[callInst] = node;
  return node;
}

FunEntryBlockNode *ICFG::getFunEntryICFGNode(const llvm::Function *F) {
  if (auto *node = getFunEntryNode(F))
    return node;
  return addFunEntryICFGNode(F);
}

FunExitBlockNode *ICFG::getFunExitICFGNode(const llvm::Function *F) {
  if (auto *node = getFunExitNode(F))
    return node;
  return addFunExitICFGNode(F);
}

CallRetBlockNode *ICFG::getRetICFGNode(const llvm::Instruction *callInst) {
  if (auto *node = getRetNode(callInst))
    return node;
  return addRetICFGNode(callInst);
}
