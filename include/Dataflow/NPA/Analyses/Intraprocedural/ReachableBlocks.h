#ifndef NPA_REACHABLE_BLOCKS_H
#define NPA_REACHABLE_BLOCKS_H

#include "Dataflow/NPA/Analyses/BitVectorSolver.h"
#include <llvm/IR/Function.h>
#include <set>

namespace npa {

/**
 * @brief Simple Reachable Blocks analysis using NPA BitVector framework.
 */
class ReachableBlocks {
public:
    static std::set<const llvm::BasicBlock*> run(llvm::Function &F, 
                                                 SolverStrategy strategy = SolverStrategy::Newton);
};

} // namespace npa

#endif // NPA_REACHABLE_BLOCKS_H
