/**
 * @file MPIRMAAnalysis.h
 * @brief MPI RMA (Remote Memory Access) Analysis
 *
 * This file provides analysis for MPI RMA operations,
 * checking for data races and synchronization errors.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_RMA_ANALYSIS_H
#define MPI_RMA_ANALYSIS_H

#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace mpi {

class MPIProcessModel;

class MPIRMAAnalysis {
public:
  enum class SyncModel { FENCE, LOCK_UNLOCK, PSCW, NONE };

  struct RMAWindow {
    WindowID window;
    const llvm::Instruction *create_inst;
    const llvm::Instruction *free_inst = nullptr;

    std::set<const llvm::Instruction *> put_ops;
    std::set<const llvm::Instruction *> get_ops;
    std::set<const llvm::Instruction *> accumulate_ops;

    std::set<const llvm::Instruction *> fence_ops;
    std::set<const llvm::Instruction *> lock_ops;
    std::set<const llvm::Instruction *> unlock_ops;
    std::set<const llvm::Instruction *> flush_ops;
  };

  struct RMAOperation {
    const llvm::Instruction *inst;
    const llvm::Function *function = nullptr;
    WindowID window;
    int target_rank = -1;
    int target_rank_min = -1;
    int target_rank_max = -1;
    int64_t target_disp = -1;
    int64_t byte_length = -1;
    RMAEpochKind rma_epoch_kind = RMAEpochKind::None;
    concurrency::ProofStrength synchronization_proof =
        concurrency::ProofStrength::Unknown;
    concurrency::Relation relation;
    SyncModel sync_model = SyncModel::NONE;
    size_t epoch_id = 0;
    bool lock_all = false;
    bool flush_completed = false;
    bool local_completion_only = false;
    bool exposure_epoch_observed = false;

    const llvm::Instruction *sync_start = nullptr;
    const llvm::Instruction *sync_end = nullptr;
  };

  MPIRMAAnalysis(const MPIProcessModel &model, ThreadAPI *api)
      : process_model_(model), thread_api_(api) {}

  void analyzeRMA();

  std::vector<RMAOperation> findUnsynchronizedRMAOps() const;

  std::vector<std::pair<RMAOperation, RMAOperation>> findRMARaces() const;

  std::vector<WindowID> findLeakedWindows() const;
  size_t getTrackedWindowCount() const { return windows_.size(); }
  const std::vector<RMAOperation> &getSynchronizationRelations() const {
    return rma_operations_;
  }

private:
  const MPIProcessModel &process_model_;
  ThreadAPI *thread_api_;

  std::map<WindowID, RMAWindow> windows_;
  std::vector<RMAOperation> rma_operations_;

  SyncModel determineSyncModel(const RMAOperation &op) const;
  bool areRMAOpsConflicting(const RMAOperation &op1,
                            const RMAOperation &op2) const;
};

} // namespace mpi

#endif // MPI_RMA_ANALYSIS_H
