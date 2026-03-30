/*
 * Copyright 2026  Lotus contributors
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
#ifndef LOTUS_ANALYSIS_LOOP_LOOPDEPENDENCEGRAPH_H
#define LOTUS_ANALYSIS_LOOP_LOOPDEPENDENCEGRAPH_H

#include "Analysis/Loop/LoopForest.h"
#include "IR/PDG/Core/Graph.h"

#include <memory>

namespace lotus {
namespace analysis {
namespace loop {

enum class LoopDependenceEdgeKind { Variable, Control, Memory };

enum class LoopDependenceMemoryKind {
  None,
  Raw,
  ReadOnly,
  AliasUnknown,
  Unknown
};

class LoopDependenceNode;
class LoopDependenceEdge;

class LoopDependenceNode {
public:
  uint64_t getID(void) const;
  Value *getValue(void) const;
  pdg::Node *getPDGNode(void) const;
  bool isInternal(void) const;
  bool isExternal(void) const;

  const std::vector<LoopDependenceEdge *> &getIncomingEdges(void) const;
  const std::vector<LoopDependenceEdge *> &getOutgoingEdges(void) const;

  bool hasIncomingEdges(void) const;
  bool hasOutgoingEdges(void) const;

private:
  friend class LoopDependenceGraph;

  LoopDependenceNode(uint64_t id, Value *value, pdg::Node *pdgNode, bool internal);

  uint64_t id;
  Value *value;
  pdg::Node *pdgNode;
  bool internal;
  std::vector<LoopDependenceEdge *> incomingEdges;
  std::vector<LoopDependenceEdge *> outgoingEdges;
};

class LoopDependenceEdge {
public:
  LoopDependenceNode *getSrc(void) const;
  LoopDependenceNode *getDst(void) const;
  LoopDependenceEdgeKind getKind(void) const;
  LoopDependenceMemoryKind getMemoryKind(void) const;
  pdg::EdgeType getOriginalEdgeType(void) const;
  bool isLoopCarried(void) const;
  void setLoopCarried(bool isLoopCarried);

private:
  friend class LoopDependenceGraph;

  LoopDependenceEdge(LoopDependenceNode *src,
                     LoopDependenceNode *dst,
                     LoopDependenceEdgeKind kind,
                     LoopDependenceMemoryKind memoryKind,
                     pdg::EdgeType originalEdgeType,
                     bool loopCarried);

  LoopDependenceNode *src;
  LoopDependenceNode *dst;
  LoopDependenceEdgeKind kind;
  LoopDependenceMemoryKind memoryKind;
  pdg::EdgeType originalEdgeType;
  bool loopCarried;
};

class LoopDependenceGraph {
public:
  LoopDependenceGraph(LoopTree *loopNode, pdg::ProgramGraph &pdg);

  LoopTree *getLoopHierarchyStructures(void) const;
  LoopStructure *getLoopStructure(void) const;

  std::vector<LoopDependenceNode *> getInternalNodes(void) const;
  std::vector<LoopDependenceNode *> getExternalNodes(void) const;
  std::vector<LoopDependenceNode *> getNodes(void) const;
  std::vector<LoopDependenceEdge *> getEdges(void) const;

  std::vector<std::pair<Value *, LoopDependenceNode *>> internalNodePairs(void) const;
  std::vector<std::pair<Value *, LoopDependenceNode *>> externalNodePairs(void) const;

  LoopDependenceNode *getNode(Value *value) const;
  LoopDependenceNode *fetchNode(Value *value) const;
  bool doesItContain(Value *value) const;
  bool isInternal(Value *value) const;
  bool isExternal(Value *value) const;

  bool iterateOverDependencesTo(
      Value *target,
      bool includeControl,
      bool includeVariable,
      bool includeMemory,
      const std::function<bool(Value *, LoopDependenceEdge *)> &funcToInvoke) const;

private:
  LoopTree *loop;
  pdg::ProgramGraph *pdg;
  std::vector<std::unique_ptr<LoopDependenceNode>> ownedNodes;
  std::vector<LoopDependenceNode *> internalNodes;
  std::vector<LoopDependenceNode *> externalNodes;
  std::vector<std::unique_ptr<LoopDependenceEdge>> ownedEdges;
  std::unordered_map<Value *, LoopDependenceNode *> nodesByValue;
  std::unordered_map<pdg::Node *, LoopDependenceNode *> nodesByPDGNode;

  LoopDependenceNode *fetchOrCreateNode(Value *value,
                                        pdg::Node *pdgNode,
                                        bool internal);
  void importEdge(pdg::Edge *edge);
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
