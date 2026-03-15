/**
 * @file MPIAnalysis.h
 * @brief MPI (Message Passing Interface) Program Analysis
 *
 * This file is the main entry point for MPI program analysis.
 * It coordinates all MPI-related analyses including process modeling,
 * collective operation checking, and RMA analysis.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_ANALYSIS_H
#define MPI_ANALYSIS_H

#include "Analysis/Concurrency/MPI/MPICollectiveAnalysis.h"
#include "Analysis/Concurrency/MPI/MPIModel.h"
#include "Analysis/Concurrency/MPI/MPIOperation.h"
#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/MPI/MPIRMAAnalysis.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace mpi {

class MPIAnalysis {
public:
  MPIAnalysis(llvm::Module &M)
      : module_(M), thread_api_(ThreadAPI::getThreadAPI()),
        process_model_(M, thread_api_), collective_analysis_(process_model_),
        rma_analysis_(process_model_, thread_api_) {}

  void runAnalysis();

  void printResults(llvm::raw_ostream &OS) const;

  struct AnalysisResults {
    std::vector<MPIProcessModel::NonBlockingOp> orphaned_requests;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        potential_deadlocks;
    std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall,
                          MPICollectiveAnalysis::CollectiveCall>>
        mismatched_collectives;
    std::vector<const llvm::Instruction *> conditional_collectives;
    std::vector<MPIRMAAnalysis::RMAOperation> unsynchronized_rma;
    std::vector<
        std::pair<MPIRMAAnalysis::RMAOperation, MPIRMAAnalysis::RMAOperation>>
        rma_races;
    std::vector<WindowID> leaked_windows;
    std::vector<const llvm::Instruction *> double_finalize;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        tag_mismatches;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        count_datatype_mismatches;
    std::vector<const llvm::Instruction *> rank_out_of_bounds;
    bool missing_finalize = false;
    std::vector<RequestID> persistent_request_leaks;
    std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall,
                          MPICollectiveAnalysis::CollectiveCall>>
        wrong_root_ranks;
    std::vector<const llvm::Instruction *> cancel_without_wait;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        buffer_overlaps;
    std::vector<const llvm::Instruction *> wildcard_in_collective;
    std::vector<const llvm::Instruction *> in_place_conflicts;
    std::vector<const llvm::Instruction *> null_handles;
    std::vector<const llvm::Instruction *> negative_root;
    std::vector<const llvm::Instruction *> invalid_tags;
    std::vector<const llvm::Instruction *> invalid_ranks;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        type_size_mismatches;
    std::vector<const llvm::Instruction *> destroy_null_comm;
    std::vector<const llvm::Instruction *> request_free_after_wait;
    std::vector<const llvm::Instruction *> in_place_wrong_op;
    std::vector<const llvm::Instruction *> invalid_rma_transitions;
    std::vector<const llvm::Instruction *> use_after_free_windows;
    std::vector<const llvm::Instruction *> double_window_free;
  };

  const AnalysisResults &getResults() const { return results_; }

  size_t getProtocolDiagnosticCount(llvm::StringRef key) const;
  size_t getOperationCount(MPIOpKind kind) const;
  size_t getTrackedWindowCount() const;

  const MPIProcessModel &getProcessModel() const { return process_model_; }
  const MPICollectiveAnalysis &getCollectiveAnalysis() const {
    return collective_analysis_;
  }
  const MPIRMAAnalysis &getRMAAnalysis() const { return rma_analysis_; }

private:
  llvm::Module &module_;
  ThreadAPI *thread_api_;

  MPIProcessModel process_model_;
  MPICollectiveAnalysis collective_analysis_;
  MPIRMAAnalysis rma_analysis_;

  AnalysisResults results_;
};

} // namespace mpi

#endif // MPI_ANALYSIS_H
