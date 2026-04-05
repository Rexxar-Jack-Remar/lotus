#pragma once

#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>

namespace llvm {

class DomTreePass {
public:
  DominatorTree &getDomTree(Function *F) {
    auto &entry = dom_trees_[F];
    if (!entry)
      entry = std::make_unique<DominatorTree>(*F);
    return *entry;
  }

  PostDominatorTree &getPostDomTree(Function *F) {
    auto &entry = post_dom_trees_[F];
    if (!entry)
      entry = std::make_unique<PostDominatorTree>(*F);
    return *entry;
  }

private:
  DenseMap<Function *, std::unique_ptr<DominatorTree>> dom_trees_;
  DenseMap<Function *, std::unique_ptr<PostDominatorTree>> post_dom_trees_;
};

} // namespace llvm
