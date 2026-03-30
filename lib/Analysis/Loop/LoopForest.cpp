/*
 * Copyright 2019 - 2025  Simone Campanoni, Lotus contributors
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
#include "Analysis/Loop/LoopForest.h"

namespace lotus {
namespace analysis {
namespace loop {

LoopTree::LoopTree(LoopForest *f, LoopStructure *l) : LoopTree(f, l, nullptr) {}

LoopTree::LoopTree(LoopForest *f, LoopStructure *l, LoopTree *parent)
    : forest{f}, loop{l}, parent{parent} {}

LoopStructure *LoopTree::getLoop(void) const { return this->loop; }

bool LoopTree::isIncludedInItsSubLoops(Instruction *inst) const {
  if (!this->loop->isIncluded(inst)) {
    return false;
  }

  for (auto *subLoopNode : this->children) {
    auto *subLoop = subLoopNode->getLoop();
    assert(subLoop != nullptr);
    if (subLoop->isIncluded(inst)) {
      return true;
    }
  }

  return false;
}

uint32_t LoopTree::getNumberOfSubLoops(void) const {
  uint32_t subloops = 0;
  for (auto *subLoop : this->children) {
    subloops++;
    subloops += subLoop->getNumberOfSubLoops();
  }
  return subloops;
}

LoopStructure *LoopTree::getInnermostLoopThatContains(Instruction *i) {
  return this->getInnermostLoopThatContains(i->getParent());
}

LoopStructure *LoopTree::getInnermostLoopThatContains(BasicBlock *bb) {
  if (!this->loop->isIncluded(bb)) {
    return nullptr;
  }

  LoopStructure *innerLoop = nullptr;
  uint32_t innerLoopLevel = 0;
  auto finder = [bb, &innerLoop, &innerLoopLevel](LoopTree *n,
                                                  uint32_t treeLevel) -> bool {
    auto *nl = n->getLoop();
    if (!nl->isIncluded(bb)) {
      return false;
    }
    if (innerLoop == nullptr || treeLevel > innerLoopLevel) {
      innerLoop = nl;
      innerLoopLevel = treeLevel;
    }
    return false;
  };
  this->visitPreOrder(finder);

  return innerLoop;
}

LoopStructure *LoopTree::getOutermostLoopThatContains(Instruction *i) {
  return this->getOutermostLoopThatContains(i->getParent());
}

LoopStructure *LoopTree::getOutermostLoopThatContains(BasicBlock *bb) {
  if (!this->loop->isIncluded(bb)) {
    return nullptr;
  }

  LoopStructure *outerLoop = nullptr;
  uint32_t outerLoopLevel = 0;
  auto finder = [bb, &outerLoop, &outerLoopLevel](LoopTree *n,
                                                  uint32_t treeLevel) -> bool {
    auto *nl = n->getLoop();
    if (!nl->isIncluded(bb)) {
      return false;
    }
    if (outerLoop == nullptr || treeLevel < outerLoopLevel) {
      outerLoop = nl;
      outerLoopLevel = treeLevel;
    }
    return false;
  };
  this->visitPreOrder(finder);

  return outerLoop;
}

LoopTree *LoopTree::getParent(void) const { return this->parent; }

std::unordered_set<LoopTree *> LoopTree::getDescendants(void) {
  std::unordered_set<LoopTree *> descendants;
  auto collector = [this, &descendants](LoopTree *n, uint32_t) -> bool {
    if (n != this) {
      descendants.insert(n);
    }
    return false;
  };
  this->visitPreOrder(collector);
  return descendants;
}

std::unordered_set<LoopTree *> LoopTree::getChildren(void) const {
  return this->children;
}

std::set<LoopTree *> LoopTree::getNodes(void) {
  std::set<LoopTree *> nodes;
  auto collector = [&nodes](LoopTree *n, uint32_t) -> bool {
    nodes.insert(n);
    return false;
  };
  this->visitPreOrder(collector);
  return nodes;
}

std::set<LoopStructure *> LoopTree::getLoops(void) {
  std::set<LoopStructure *> loops;
  auto collector = [&loops](LoopTree *n, uint32_t) -> bool {
    loops.insert(n->getLoop());
    return false;
  };
  this->visitPreOrder(collector);
  return loops;
}

bool LoopTree::visitPreOrder(
    std::function<bool(LoopTree *n, uint32_t treeLevel)> funcToInvoke) {
  return this->visitPreOrder(funcToInvoke, 1);
}

bool LoopTree::visitPostOrder(
    std::function<bool(LoopTree *n, uint32_t treeLevel)> funcToInvoke) {
  return this->visitPostOrder(funcToInvoke, 1);
}

bool LoopTree::visitPreOrder(
    std::function<bool(LoopTree *n, uint32_t treeLevel)> funcToInvoke,
    uint32_t treeLevel) {
  if (funcToInvoke(this, treeLevel)) {
    return true;
  }

  for (auto *child : this->children) {
    if (child->visitPreOrder(funcToInvoke, treeLevel + 1)) {
      return true;
    }
  }

  return false;
}

bool LoopTree::visitPostOrder(
    std::function<bool(LoopTree *n, uint32_t treeLevel)> funcToInvoke,
    uint32_t treeLevel) {
  for (auto *child : this->children) {
    if (child->visitPostOrder(funcToInvoke, treeLevel + 1)) {
      return true;
    }
  }

  return funcToInvoke(this, treeLevel);
}

LoopTree::~LoopTree() {
  if (this->parent != nullptr) {
    this->parent->children.erase(this);
    for (auto *child : this->children) {
      child->parent = this->parent;
      this->parent->children.insert(child);
    }
    return;
  }

  this->forest->removeTree(this);
}

LoopForest::LoopForest(
    std::vector<LoopStructure *> const &loops,
    std::unordered_map<Function *, noelle::DominatorSummary *> const &doms) {
  std::unordered_set<LoopTree *> potentialTrees;
  for (auto *l : loops) {
    auto *func = l->getFunction();
    auto *header = l->getHeader();
    auto *n = new LoopTree(this, l);
    this->nodes[l] = n;
    this->functionLoops[func].insert(l);
    this->headerLoops[header] = n;

    if (l->getNestingLevel() == 1) {
      this->trees.insert(n);
    } else {
      potentialTrees.insert(n);
    }
  }

  for (auto *t : this->trees) {
    this->addChildrenToTree(t, doms, potentialTrees);
  }

  for (auto *n : potentialTrees) {
    this->trees.insert(n);
  }
}

uint64_t LoopForest::getNumberOfLoops(void) const {
  uint64_t total = 0;
  for (auto *tree : this->getTrees()) {
    total++;
    total += tree->getNumberOfSubLoops();
  }
  return total;
}

void LoopForest::addChildrenToTree(
    LoopTree *root,
    std::unordered_map<Function *, noelle::DominatorSummary *> const &doms,
    std::unordered_set<LoopTree *> &potentialTrees) {
  auto *l = root->getLoop();
  auto *header = l->getHeader();
  auto *func = l->getFunction();

  auto *ds = doms.at(func);
  auto *loopHeaderDominatorNode = ds->DT.getNode(header);

  for (auto *functionLoop : this->functionLoops[func]) {
    auto *functionLoopHeader = functionLoop->getHeader();
    if (functionLoopHeader == header) {
      continue;
    }
    if (!l->isIncluded(functionLoopHeader)) {
      continue;
    }

    auto *subLoopDominatorNode = ds->DT.getNode(functionLoopHeader);
    assert(loopHeaderDominatorNode != subLoopDominatorNode);
    subLoopDominatorNode = subLoopDominatorNode->getParent();

    bool foundLoopInBetween = false;
    while (loopHeaderDominatorNode != subLoopDominatorNode) {
      auto *bb = subLoopDominatorNode->getBlock();
      auto it = this->headerLoops.find(bb);
      if (it != this->headerLoops.end()) {
        auto *loopInBetween = it->second;
        auto *lsLoopInBetween = loopInBetween->getLoop();
        if (lsLoopInBetween->isIncluded(functionLoopHeader)) {
          foundLoopInBetween = true;
          break;
        }
      }
      subLoopDominatorNode = subLoopDominatorNode->getParent();
    }
    if (foundLoopInBetween) {
      continue;
    }

    auto *child = this->nodes[functionLoop];
    assert(child != nullptr);
    root->children.insert(child);
    child->parent = root;
    potentialTrees.erase(child);
    this->addChildrenToTree(child, doms, potentialTrees);
  }
}

std::unordered_set<LoopTree *> LoopForest::getTrees(void) const {
  return this->trees;
}

void LoopForest::removeTree(LoopTree *tree) { this->trees.erase(tree); }

void LoopForest::addTree(LoopTree *tree) { this->trees.insert(tree); }

LoopForest::~LoopForest() {
  for (auto &pair : this->nodes) {
    delete pair.second;
  }
}

LoopTree *LoopForest::getNode(LoopStructure *loop) const {
  auto it = this->headerLoops.find(loop->getHeader());
  if (it == this->headerLoops.end()) {
    return nullptr;
  }
  return it->second;
}

LoopTree *LoopForest::getInnermostLoopThatContains(Instruction *i) const {
  for (auto *tree : this->getTrees()) {
    auto *ls = tree->getLoop();
    if (ls->getFunction() != i->getFunction()) {
      continue;
    }
    if (!ls->isIncluded(i)) {
      continue;
    }

    LoopTree *innermostLoop = nullptr;
    auto finder = [i, &innermostLoop](LoopTree *n, uint32_t) -> bool {
      if (n->getLoop()->isIncluded(i)) {
        innermostLoop = n;
        return true;
      }
      return false;
    };
    tree->visitPostOrder(finder);
    return innermostLoop;
  }

  return nullptr;
}

LoopTree *LoopForest::getInnermostLoopThatContains(BasicBlock *bb) const {
  return bb->empty() ? nullptr : this->getInnermostLoopThatContains(&*bb->begin());
}

} // namespace loop
} // namespace analysis
} // namespace lotus
