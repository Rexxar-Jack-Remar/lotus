/** @file SortTopo.h @brief Topological sorting utilities for CFG nodes. */
#ifndef __SORT_TOPO_HH_
#define __SORT_TOPO_HH_

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/Function.h"

void RevTopoSort(const llvm::Function &F,
                 std::vector<const llvm::BasicBlock *> &out);

#endif
