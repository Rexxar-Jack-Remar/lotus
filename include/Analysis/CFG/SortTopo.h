/** @file SortTopo.h @brief Topological sorting utilities for CFG nodes. */
#pragma once

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/Function.h"

void RevTopoSort(const llvm::Function &F,
                 std::vector<const llvm::BasicBlock *> &out);

