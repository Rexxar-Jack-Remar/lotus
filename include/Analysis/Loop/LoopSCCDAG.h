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
#ifndef LOTUS_ANALYSIS_LOOP_LOOPSCCDAG_H
#define LOTUS_ANALYSIS_LOOP_LOOPSCCDAG_H

#include "Analysis/Loop/LoopDependenceGraph.h"

namespace lotus {
namespace analysis {
namespace loop {

class LoopSCC {
public:
  uint64_t getID(void) const;
  bool hasCycle(void) const;

  std::vector<LoopDependenceNode *> getNodes(void) const;
  std::vector<LoopSCC *> getPredecessors(void) const;
  std::vector<LoopSCC *> getSuccessors(void) const;
  std::vector<LoopDependenceEdge *> getEdges(void) const;

  std::vector<std::pair<Value *, LoopDependenceNode *>> internalNodePairs(void) const;
  std::vector<std::pair<Value *, LoopDependenceNode *>> externalNodePairs(void) const;
  bool isInternal(Value *value) const;
  bool isExternal(Value *value) const;
  LoopDependenceNode *fetchNode(Value *value) const;

  int64_t numberOfInstructions(void) const;
  bool iterateOverInstructions(
      const std::function<bool(Instruction *)> &funcToInvoke) const;
  bool iterateOverAllInstructions(
      const std::function<bool(Instruction *)> &funcToInvoke) const;
  bool iterateOverValues(
      const std::function<bool(Value *)> &funcToInvoke) const;
  bool iterateOverAllValues(
      const std::function<bool(Value *)> &funcToInvoke) const;

private:
  friend class LoopSCCDAG;

  LoopSCC(uint64_t id, std::vector<LoopDependenceNode *> members, bool hasCycle);

  uint64_t id;
  std::vector<LoopDependenceNode *> internalNodes;
  std::vector<LoopDependenceNode *> externalNodes;
  std::vector<LoopSCC *> predecessors;
  std::vector<LoopSCC *> successors;
  bool cycle;
};

class LoopSCCDAG {
public:
  explicit LoopSCCDAG(LoopDependenceGraph &graph);

  LoopDependenceGraph *getLoopDependenceGraph(void) const;

  std::vector<LoopSCC *> getSCCs(void) const;
  std::vector<LoopSCC *> getAllSCCs(void) const;
  LoopSCC *getSCC(Value *value) const;
  bool orderedBefore(const LoopSCC *early, const LoopSCC *late) const;

private:
  LoopDependenceGraph *graph;
  std::vector<LoopDependenceNode *> nodes;
  std::vector<std::unique_ptr<LoopSCC>> ownedSCCs;
  std::vector<std::unique_ptr<LoopSCC>> ownedExternalSCCs;
  std::unordered_map<LoopDependenceNode *, LoopSCC *> sccByNode;
  std::unordered_map<Value *, LoopSCC *> sccByValue;
  std::unordered_map<const LoopSCC *, uint32_t> sccIndexes;
  std::unordered_map<const LoopSCC *, std::unordered_set<const LoopSCC *>>
      reachableSCCs;

  void computeReachabilityAmongSCCs(void);
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
