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

void strongConnect(LoopDependenceNode *node,
                   TarjanState &state,
                   std::vector<std::vector<LoopDependenceNode *>> &components) {
  state.index[node] = state.nextIndex;
  state.lowlink[node] = state.nextIndex;
  state.nextIndex++;
  state.stack.push_back(node);
  state.onStack.insert(node);

  for (auto *edge : node->getOutgoingEdges()) {
    auto *succ = edge->getDst();
    if (succ == nullptr || succ->isExternal()) {
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

  std::sort(component.begin(),
            component.end(),
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

LoopSCC::LoopSCC(uint64_t id,
                 std::vector<LoopDependenceNode *> members,
                 bool hasCycle)
    : id{id}, nodes{std::move(members)}, cycle{hasCycle} {}

uint64_t LoopSCC::getID(void) const { return this->id; }

bool LoopSCC::hasCycle(void) const { return this->cycle; }

std::vector<LoopDependenceNode *> LoopSCC::getNodes(void) const {
  return this->nodes;
}

std::vector<LoopSCC *> LoopSCC::getPredecessors(void) const {
  return this->predecessors;
}

std::vector<LoopSCC *> LoopSCC::getSuccessors(void) const {
  return this->successors;
}

LoopSCCDAG::LoopSCCDAG(LoopDependenceGraph &graph) : graph{&graph} {
  TarjanState state;
  std::vector<std::vector<LoopDependenceNode *>> components;

  auto internalNodes = graph.getInternalNodes();
  std::sort(internalNodes.begin(),
            internalNodes.end(),
            [](LoopDependenceNode *lhs, LoopDependenceNode *rhs) {
              return lhs->getID() < rhs->getID();
            });

  for (auto *node : internalNodes) {
    if (state.index.find(node) == state.index.end()) {
      strongConnect(node, state, components);
    }
  }

  std::sort(components.begin(),
            components.end(),
            [](std::vector<LoopDependenceNode *> const &lhs,
               std::vector<LoopDependenceNode *> const &rhs) {
              return lhs.front()->getID() < rhs.front()->getID();
            });

  for (auto &component : components) {
    auto owned =
        std::unique_ptr<LoopSCC>(new LoopSCC(this->ownedSCCs.size(),
                                             component,
                                             componentHasCycle(component)));
    auto *raw = owned.get();
    this->ownedSCCs.push_back(std::move(owned));
    for (auto *node : raw->nodes) {
      this->sccByNode[node] = raw;
      auto *value = node->getValue();
      if (value != nullptr) {
        this->sccByValue[value] = raw;
      }
    }
  }

  std::set<std::pair<LoopSCC *, LoopSCC *>> seenPairs;
  for (auto *node : internalNodes) {
    auto *srcSCC = this->sccByNode.at(node);
    for (auto *edge : node->getOutgoingEdges()) {
      auto *dst = edge->getDst();
      if (dst == nullptr || dst->isExternal()) {
        continue;
      }
      auto *dstSCC = this->sccByNode.at(dst);
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
    std::sort(ownedSCC->predecessors.begin(),
              ownedSCC->predecessors.end(),
              [](LoopSCC *lhs, LoopSCC *rhs) { return lhs->getID() < rhs->getID(); });
    std::sort(ownedSCC->successors.begin(),
              ownedSCC->successors.end(),
              [](LoopSCC *lhs, LoopSCC *rhs) { return lhs->getID() < rhs->getID(); });
  }
}

LoopDependenceGraph *LoopSCCDAG::getLoopDependenceGraph(void) const {
  return this->graph;
}

std::vector<LoopSCC *> LoopSCCDAG::getSCCs(void) const {
  std::vector<LoopSCC *> sccs;
  sccs.reserve(this->ownedSCCs.size());
  for (auto const &owned : this->ownedSCCs) {
    sccs.push_back(owned.get());
  }
  return sccs;
}

LoopSCC *LoopSCCDAG::getSCC(Value *value) const {
  auto it = this->sccByValue.find(value);
  if (it == this->sccByValue.end()) {
    return nullptr;
  }
  return it->second;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
