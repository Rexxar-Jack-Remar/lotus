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
#include "Analysis/Loop/LoopSCCDAG.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

struct TarjanState {
  uint64_t nextIndex{0};
  std::unordered_map<LoopDependenceNode *, uint64_t> index;
  std::unordered_map<LoopDependenceNode *, uint64_t> lowlink;
  std::unordered_set<LoopDependenceNode *> onStack;
  std::vector<LoopDependenceNode *> stack;
};

void strongConnect(LoopDependenceNode *node, TarjanState &state,
                   std::vector<std::vector<LoopDependenceNode *>> &components) {
  state.index[node] = state.nextIndex;
  state.lowlink[node] = state.nextIndex;
  state.nextIndex++;
  state.stack.push_back(node);
  state.onStack.insert(node);

  for (auto *edge : node->getOutgoingEdges()) {
    auto *succ = edge->getDst();
    if (succ == nullptr) {
      continue;
    }

    if (state.index.find(succ) == state.index.end()) {
      strongConnect(succ, state, components);
      state.lowlink[node] = std::min(state.lowlink[node], state.lowlink[succ]);
    } else if (state.onStack.count(succ) != 0) {
      state.lowlink[node] = std::min(state.lowlink[node], state.index[succ]);
    }
  }

  if (state.lowlink[node] != state.index[node]) {
    return;
  }

  std::vector<LoopDependenceNode *> component;
  while (!state.stack.empty()) {
    auto *current = state.stack.back();
    state.stack.pop_back();
    state.onStack.erase(current);
    component.push_back(current);
    if (current == node) {
      break;
    }
  }

  std::sort(component.begin(), component.end(),
            [](LoopDependenceNode *lhs, LoopDependenceNode *rhs) {
              return lhs->getID() < rhs->getID();
            });
  components.push_back(component);
}

bool componentHasCycle(std::vector<LoopDependenceNode *> const &members) {
  if (members.size() > 1) {
    return true;
  }

  auto *single = members.front();
  for (auto *edge : single->getOutgoingEdges()) {
    if (edge->getDst() == single) {
      return true;
    }
  }
  return false;
}

} // namespace

LoopSCC::LoopSCC(uint64_t id, std::vector<LoopDependenceNode *> members,
                 bool hasCycle, bool includedInLoop)
    : id{id}, internalNodes{std::move(members)}, externalNodes{},
      cycle{hasCycle}, includedInLoop{includedInLoop} {}

uint64_t LoopSCC::getID(void) const { return this->id; }

bool LoopSCC::hasCycle(void) const { return this->cycle; }

bool LoopSCC::isIncludedInLoop(void) const { return this->includedInLoop; }

std::vector<LoopDependenceNode *> LoopSCC::getNodes(void) const {
  std::vector<LoopDependenceNode *> nodes;
  nodes.reserve(this->internalNodes.size() + this->externalNodes.size());
  nodes.insert(nodes.end(), this->internalNodes.begin(),
               this->internalNodes.end());
  nodes.insert(nodes.end(), this->externalNodes.begin(),
               this->externalNodes.end());
  return nodes;
}

std::vector<LoopSCC *> LoopSCC::getPredecessors(void) const {
  return this->predecessors;
}

std::vector<LoopSCC *> LoopSCC::getSuccessors(void) const {
  return this->successors;
}

std::vector<LoopDependenceEdge *> LoopSCC::getEdges(void) const {
  std::vector<LoopDependenceEdge *> edges;
  std::set<LoopDependenceEdge *> seen;
  for (auto *node : this->internalNodes) {
    for (auto *edge : node->getOutgoingEdges()) {
      auto *dst = edge->getDst();
      if (dst == nullptr) {
        continue;
      }
      bool srcInside =
          std::find(this->internalNodes.begin(), this->internalNodes.end(),
                    node) != this->internalNodes.end();
      bool dstInside =
          std::find(this->internalNodes.begin(), this->internalNodes.end(),
                    dst) != this->internalNodes.end();
      if (srcInside && dstInside && seen.insert(edge).second) {
        edges.push_back(edge);
      }
    }
  }
  return edges;
}

std::vector<std::pair<Value *, LoopDependenceNode *>>
LoopSCC::internalNodePairs(void) const {
  std::vector<std::pair<Value *, LoopDependenceNode *>> pairs;
  for (auto *node : this->internalNodes) {
    pairs.emplace_back(node->getValue(), node);
  }
  return pairs;
}

std::vector<std::pair<Value *, LoopDependenceNode *>>
LoopSCC::externalNodePairs(void) const {
  std::vector<std::pair<Value *, LoopDependenceNode *>> pairs;
  for (auto *node : this->externalNodes) {
    pairs.emplace_back(node->getValue(), node);
  }
  return pairs;
}

bool LoopSCC::isInternal(Value *value) const {
  for (auto *node : this->internalNodes) {
    if (node->getValue() == value) {
      return true;
    }
  }
  return false;
}

bool LoopSCC::isExternal(Value *value) const {
  for (auto *node : this->externalNodes) {
    if (node->getValue() == value) {
      return true;
    }
  }
  return false;
}

LoopDependenceNode *LoopSCC::fetchNode(Value *value) const {
  for (auto *node : this->internalNodes) {
    if (node->getValue() == value) {
      return node;
    }
  }
  for (auto *node : this->externalNodes) {
    if (node->getValue() == value) {
      return node;
    }
  }
  return nullptr;
}

int64_t LoopSCC::numberOfInstructions(void) const {
  int64_t count = 0;
  for (auto *node : this->internalNodes) {
    if (isa<Instruction>(node->getValue())) {
      ++count;
    }
  }
  return count;
}

bool LoopSCC::iterateOverInstructions(
    const std::function<bool(Instruction *)> &funcToInvoke) const {
  for (auto *node : this->internalNodes) {
    if (auto *inst = dyn_cast_or_null<Instruction>(node->getValue())) {
      if (funcToInvoke(inst)) {
        return true;
      }
    }
  }
  return false;
}

bool LoopSCC::iterateOverAllInstructions(
    const std::function<bool(Instruction *)> &funcToInvoke) const {
  for (auto *node : this->getNodes()) {
    if (auto *inst = dyn_cast_or_null<Instruction>(node->getValue())) {
      if (funcToInvoke(inst)) {
        return true;
      }
    }
  }
  return false;
}

bool LoopSCC::iterateOverValues(
    const std::function<bool(Value *)> &funcToInvoke) const {
  for (auto *node : this->internalNodes) {
    if (funcToInvoke(node->getValue())) {
      return true;
    }
  }
  return false;
}

bool LoopSCC::iterateOverAllValues(
    const std::function<bool(Value *)> &funcToInvoke) const {
  for (auto *node : this->getNodes()) {
    if (funcToInvoke(node->getValue())) {
      return true;
    }
  }
  return false;
}

LoopSCCDAG::LoopSCCDAG(LoopDependenceGraph &graph) : graph{&graph} {
  TarjanState state;
  std::vector<std::vector<LoopDependenceNode *>> components;

  this->nodes = graph.getNodes();
  std::sort(this->nodes.begin(), this->nodes.end(),
            [](LoopDependenceNode *lhs, LoopDependenceNode *rhs) {
              return lhs->getID() < rhs->getID();
            });

  for (auto *node : this->nodes) {
    if (state.index.find(node) == state.index.end()) {
      strongConnect(node, state, components);
    }
  }

  std::sort(components.begin(), components.end(),
            [](std::vector<LoopDependenceNode *> const &lhs,
               std::vector<LoopDependenceNode *> const &rhs) {
              return lhs.front()->getID() < rhs.front()->getID();
            });

  for (auto &component : components) {
    bool includedInLoop = false;
    for (auto *node : component) {
      includedInLoop |= node->isInternal();
    }
    auto owned = std::unique_ptr<LoopSCC>(new LoopSCC(
        this->ownedSCCs.size(), component, componentHasCycle(component),
        includedInLoop));
    auto *raw = owned.get();
    this->ownedSCCs.push_back(std::move(owned));
    if (includedInLoop) {
      this->includedSCCs.push_back(raw);
    }
    for (auto *node : raw->internalNodes) {
      this->sccByNode[node] = raw;
      auto *value = node->getValue();
      if (value != nullptr) {
        this->sccByValue[value] = raw;
      }
    }
  }

  for (auto &ownedSCC : this->ownedSCCs) {
    auto *scc = ownedSCC.get();
    std::unordered_set<LoopDependenceNode *> seenExternal;
    for (auto *node : scc->internalNodes) {
      seenExternal.insert(node);
    }

    for (auto *node : scc->internalNodes) {
      for (auto *edge : node->getOutgoingEdges()) {
        auto *dst = edge->getDst();
        if (dst == nullptr || !seenExternal.insert(dst).second) {
          continue;
        }
        scc->externalNodes.push_back(dst);
      }
      for (auto *edge : node->getIncomingEdges()) {
        auto *src = edge->getSrc();
        if (src == nullptr || !seenExternal.insert(src).second) {
          continue;
        }
        scc->externalNodes.push_back(src);
      }
    }

    std::sort(scc->externalNodes.begin(), scc->externalNodes.end(),
              [](LoopDependenceNode *lhs, LoopDependenceNode *rhs) {
                return lhs->getID() < rhs->getID();
              });
  }

  std::set<std::pair<LoopSCC *, LoopSCC *>> seenPairs;
  auto allNodes = graph.getNodes();
  for (auto *node : allNodes) {
    auto srcIt = this->sccByNode.find(node);
    if (srcIt == this->sccByNode.end()) {
      continue;
    }
    auto *srcSCC = srcIt->second;
    for (auto *edge : node->getOutgoingEdges()) {
      auto *dst = edge->getDst();
      if (dst == nullptr) {
        continue;
      }
      auto dstIt = this->sccByNode.find(dst);
      if (dstIt == this->sccByNode.end()) {
        continue;
      }
      auto *dstSCC = dstIt->second;
      if (srcSCC == dstSCC) {
        continue;
      }
      if (!seenPairs.insert(std::make_pair(srcSCC, dstSCC)).second) {
        continue;
      }
      srcSCC->successors.push_back(dstSCC);
      dstSCC->predecessors.push_back(srcSCC);
    }
  }

  for (auto &ownedSCC : this->ownedSCCs) {
    std::sort(
        ownedSCC->predecessors.begin(), ownedSCC->predecessors.end(),
        [](LoopSCC *lhs, LoopSCC *rhs) { return lhs->getID() < rhs->getID(); });
    std::sort(
        ownedSCC->successors.begin(), ownedSCC->successors.end(),
        [](LoopSCC *lhs, LoopSCC *rhs) { return lhs->getID() < rhs->getID(); });
  }
  this->computeReachabilityAmongSCCs();
}

LoopDependenceGraph *LoopSCCDAG::getLoopDependenceGraph(void) const {
  return this->graph;
}

std::vector<LoopSCC *> LoopSCCDAG::getSCCs(void) const {
  std::vector<LoopSCC *> sccs;
  sccs.reserve(this->includedSCCs.size());
  sccs.insert(sccs.end(), this->includedSCCs.begin(), this->includedSCCs.end());
  std::sort(sccs.begin(), sccs.end(),
            [](LoopSCC *lhs, LoopSCC *rhs) { return lhs->getID() < rhs->getID(); });
  return sccs;
}

std::vector<LoopSCC *> LoopSCCDAG::getAllSCCs(void) const {
  std::vector<LoopSCC *> sccs;
  sccs.reserve(this->ownedSCCs.size());
  for (auto const &owned : this->ownedSCCs) {
    sccs.push_back(owned.get());
  }
  std::sort(sccs.begin(), sccs.end(), [](LoopSCC *lhs, LoopSCC *rhs) {
    return lhs->getID() < rhs->getID();
  });
  return sccs;
}

LoopSCC *LoopSCCDAG::getSCC(Value *value) const {
  auto it = this->sccByValue.find(value);
  if (it == this->sccByValue.end()) {
    return nullptr;
  }
  return it->second;
}

bool LoopSCCDAG::orderedBefore(const LoopSCC *early,
                               const LoopSCC *late) const {
  auto it = this->reachableSCCs.find(early);
  if (it == this->reachableSCCs.end()) {
    return false;
  }
  return it->second.count(late) != 0;
}

void LoopSCCDAG::computeReachabilityAmongSCCs(void) {
  this->reachableSCCs.clear();
  this->sccIndexes.clear();
  uint32_t index = 0;
  for (auto const &owned : this->ownedSCCs) {
    this->sccIndexes.emplace(owned.get(), index++);
  }

  for (auto const &owned : this->ownedSCCs) {
    auto *root = owned.get();
    std::queue<const LoopSCC *> queue;
    std::unordered_set<const LoopSCC *> visited;
    for (auto *succ : root->getSuccessors()) {
      queue.push(succ);
    }
    while (!queue.empty()) {
      auto *current = queue.front();
      queue.pop();
      if (!visited.insert(current).second) {
        continue;
      }
      this->reachableSCCs[root].insert(current);
      for (auto *succ : current->getSuccessors()) {
        queue.push(succ);
      }
    }
  }
}

} // namespace loop
} // namespace analysis
} // namespace lotus
