#pragma once

#include "Analysis/Concurrency/MPI/MPIAnalysis.h"
#include "Analysis/Concurrency/OpenMP/OpenMPTaskGraph.h"

#include <llvm/IR/Module.h>

namespace concurrency {

class ConcurrencyFacade {
public:
  struct OpenMPSummary {
    size_t task_count = 0;
    size_t wait_boundary_count = 0;
    size_t unknown_relation_count = 0;
    size_t deferred_wait_dep_count = 0;
    size_t deferred_conflict_count = 0;
  };

  struct MPISummary {
    size_t operation_count = 0;
    size_t orphaned_request_count = 0;
    size_t potential_deadlock_count = 0;
    size_t mismatched_collective_count = 0;
    size_t conditional_collective_count = 0;
    size_t rma_race_count = 0;
  };

  static OpenMPSummary analyzeOpenMP(llvm::Module &module);
  static MPISummary analyzeMPI(llvm::Module &module);
};

} // namespace concurrency
