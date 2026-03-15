/**
 * @file MPIRMAAnalysis.cpp
 * @brief MPI RMA Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPIRMAAnalysis.h"

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

using namespace llvm;

namespace mpi {

namespace {

bool rangesOverlap(int lhs_min, int lhs_max, int rhs_min, int rhs_max) {
  if (lhs_min < 0 || lhs_max < 0 || rhs_min < 0 || rhs_max < 0) {
    return true;
  }
  return !(lhs_max < rhs_min || rhs_max < lhs_min);
}

} // namespace

void MPIRMAAnalysis::analyzeRMA() {
  windows_.clear();
  rma_operations_.clear();

  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.td_type == ThreadAPI::TD_MPI_WIN_CREATE) {
      RMAWindow window;
      window.window = nullptr;
      window.create_inst = op.inst;

      const CallBase *CB = dyn_cast<CallBase>(op.inst);
      if (CB && CB->arg_size() > 0) {
        window.window = CB->getArgOperand(CB->arg_size() - 1);
        windows_[window.window] = window;
      }
    } else if (op.td_type == ThreadAPI::TD_MPI_WIN_FREE) {
      const CallBase *CB = dyn_cast<CallBase>(op.inst);
      if (CB && CB->arg_size() > 0) {
        WindowID win = CB->getArgOperand(0);
        auto it = windows_.find(win);
        if (it != windows_.end()) {
          it->second.free_inst = op.inst;
        }
      }
    }
  }

  struct PendingEpoch {
    SyncModel model = SyncModel::NONE;
    const Instruction *start = nullptr;
    std::vector<size_t> op_indices;
    size_t epoch_id = 0;
  };

  std::map<std::pair<const Function *, WindowID>, PendingEpoch> pending_epochs;
  std::map<std::pair<const Function *, WindowID>, const Instruction *>
      last_fence_by_window;
  size_t next_epoch_id = 1;

  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.kind == MPIOpKind::RMA_DATA) {
      RMAOperation rma_op;
      rma_op.inst = op.inst;
      rma_op.function = op.function;
      rma_op.window = op.window;
      rma_op.target_rank = op.target_rank;
      rma_op.target_rank_min = op.target_rank_min;
      rma_op.target_rank_max = op.target_rank_max;
      rma_op.target_disp = op.target_disp;
      rma_op.byte_length = op.byte_length;
      rma_op.rma_epoch_kind = RMAEpochKind::Access;
      rma_op.lock_all = op.td_type == ThreadAPI::TD_MPI_WIN_LOCK &&
                        op.target_rank < 0;

      size_t op_index = rma_operations_.size();
      rma_operations_.push_back(rma_op);

      auto key = std::make_pair(op.function, op.window);
      auto epoch_it = pending_epochs.find(key);
      if (epoch_it != pending_epochs.end() &&
          epoch_it->second.model != SyncModel::NONE) {
        epoch_it->second.op_indices.push_back(op_index);
      }

      auto it = windows_.find(op.window);
      if (it != windows_.end()) {
        if (op.td_type == ThreadAPI::TD_MPI_PUT) {
          it->second.put_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_GET) {
          it->second.get_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_ACCUMULATE) {
          it->second.accumulate_ops.insert(op.inst);
        }
      }
    } else if (op.kind == MPIOpKind::RMA_SYNC) {
      auto it = windows_.find(op.window);
      if (it != windows_.end()) {
        if (op.td_type == ThreadAPI::TD_MPI_WIN_FENCE) {
          it->second.fence_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_LOCK) {
          it->second.lock_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_UNLOCK) {
          it->second.unlock_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_FLUSH) {
          it->second.flush_ops.insert(op.inst);
        }
      }

      auto key = std::make_pair(op.function, op.window);
      PendingEpoch &epoch = pending_epochs[key];
      switch (op.td_type) {
      case ThreadAPI::TD_MPI_WIN_FENCE: {
        auto fence_it = last_fence_by_window.find(key);
        if (fence_it != last_fence_by_window.end()) {
          for (size_t idx : epoch.op_indices) {
            rma_operations_[idx].sync_model = SyncModel::FENCE;
            rma_operations_[idx].sync_start = fence_it->second;
            rma_operations_[idx].sync_end = op.inst;
            rma_operations_[idx].epoch_id = epoch.epoch_id;
            rma_operations_[idx].synchronization_proof =
                concurrency::ProofStrength::Must;
            rma_operations_[idx].relation.kind =
                concurrency::RelationKind::SameSynchronizationEpoch;
            rma_operations_[idx].relation.proof =
                concurrency::ProofStrength::Must;
            rma_operations_[idx].relation.reason = "mpi_rma_fence_epoch";
          }
          epoch.op_indices.clear();
        }
        epoch.model = SyncModel::FENCE;
        epoch.start = op.inst;
        epoch.epoch_id = next_epoch_id++;
        last_fence_by_window[key] = op.inst;
        break;
      }
      case ThreadAPI::TD_MPI_WIN_LOCK:
        epoch.model = SyncModel::LOCK_UNLOCK;
        epoch.start = op.inst;
        epoch.epoch_id = next_epoch_id++;
        epoch.op_indices.clear();
        break;
      case ThreadAPI::TD_MPI_WIN_UNLOCK:
        if (epoch.model == SyncModel::LOCK_UNLOCK && epoch.start) {
          for (size_t idx : epoch.op_indices) {
            rma_operations_[idx].sync_model = SyncModel::LOCK_UNLOCK;
            rma_operations_[idx].sync_start = epoch.start;
            rma_operations_[idx].sync_end = op.inst;
            rma_operations_[idx].epoch_id = epoch.epoch_id;
            rma_operations_[idx].synchronization_proof =
                concurrency::ProofStrength::Must;
            rma_operations_[idx].relation.kind =
                concurrency::RelationKind::SameSynchronizationEpoch;
            rma_operations_[idx].relation.proof =
                concurrency::ProofStrength::Must;
            rma_operations_[idx].relation.reason = "mpi_rma_lock_epoch";
          }
        }
        epoch.model = SyncModel::NONE;
        epoch.start = nullptr;
        epoch.epoch_id = 0;
        epoch.op_indices.clear();
        break;
      case ThreadAPI::TD_MPI_WIN_FLUSH:
      case ThreadAPI::TD_MPI_WIN_SYNC:
        for (size_t idx : epoch.op_indices) {
          rma_operations_[idx].flush_completed = true;
          rma_operations_[idx].local_completion_only =
              op.rma_local_completion_only;
          if (op.rma_local_completion_only) {
            rma_operations_[idx].relation.kind =
                concurrency::RelationKind::UnknownDueToModelGap;
            rma_operations_[idx].relation.proof =
                concurrency::ProofStrength::May;
            rma_operations_[idx].relation.reason =
                "mpi_rma_flush_local_completion";
            continue;
          }
          rma_operations_[idx].relation.kind =
              concurrency::RelationKind::SameSynchronizationEpoch;
          rma_operations_[idx].relation.proof = concurrency::ProofStrength::May;
          rma_operations_[idx].relation.reason =
              op.td_type == ThreadAPI::TD_MPI_WIN_SYNC
                  ? "mpi_rma_sync_completion"
                  : "mpi_rma_flush_completion";
        }
        break;
      case ThreadAPI::TD_MPI_WIN_START:
        epoch.model = SyncModel::PSCW;
        epoch.start = op.inst;
        epoch.epoch_id = next_epoch_id++;
        epoch.op_indices.clear();
        break;
      case ThreadAPI::TD_MPI_WIN_COMPLETE:
        if (epoch.model == SyncModel::PSCW && epoch.start) {
          for (size_t idx : epoch.op_indices) {
            rma_operations_[idx].sync_model = SyncModel::PSCW;
            rma_operations_[idx].sync_start = epoch.start;
            rma_operations_[idx].sync_end = op.inst;
            rma_operations_[idx].epoch_id = epoch.epoch_id;
            rma_operations_[idx].synchronization_proof =
                concurrency::ProofStrength::Must;
            rma_operations_[idx].relation.kind =
                concurrency::RelationKind::SameSynchronizationEpoch;
            rma_operations_[idx].relation.proof =
                concurrency::ProofStrength::Must;
            rma_operations_[idx].relation.reason = "mpi_rma_pscw_epoch";
          }
        }
        epoch.model = SyncModel::NONE;
        epoch.start = nullptr;
        epoch.epoch_id = 0;
        epoch.op_indices.clear();
        break;
      case ThreadAPI::TD_MPI_WIN_POST:
      case ThreadAPI::TD_MPI_WIN_WAIT:
      case ThreadAPI::TD_MPI_WIN_TEST:
        for (size_t idx : epoch.op_indices) {
          rma_operations_[idx].exposure_epoch_observed = true;
        }
        break;
      default:
        break;
      }
    }
  }
}

MPIRMAAnalysis::SyncModel
MPIRMAAnalysis::determineSyncModel(const RMAOperation &op) const {
  return op.sync_model;
}

bool MPIRMAAnalysis::areRMAOpsConflicting(const RMAOperation &op1,
                                          const RMAOperation &op2) const {
  if (op1.window != op2.window)
    return false;
  if (!rangesOverlap(op1.target_rank, op1.target_rank, op2.target_rank,
                     op2.target_rank) &&
      !rangesOverlap(op1.target_rank_min, op1.target_rank_max,
                     op2.target_rank_min, op2.target_rank_max)) {
    return false;
  }
  if (op1.target_disp != -1 && op2.target_disp != -1) {
    int64_t len1 = op1.byte_length > 0 ? op1.byte_length : 1;
    int64_t len2 = op2.byte_length > 0 ? op2.byte_length : 1;
    int64_t end1 = op1.target_disp + len1;
    int64_t end2 = op2.target_disp + len2;
    if (!(op1.target_disp < end2 && op2.target_disp < end1)) {
      return false;
    }
  }

  const CallBase *CB1 = dyn_cast<CallBase>(op1.inst);
  const CallBase *CB2 = dyn_cast<CallBase>(op2.inst);
  if (!CB1 || !CB2)
    return false;

  const Function *F1 = CB1->getCalledFunction();
  const Function *F2 = CB2->getCalledFunction();
  if (!F1 || !F2)
    return false;

  ThreadAPI::TD_TYPE t1 = thread_api_->getType(F1);
  ThreadAPI::TD_TYPE t2 = thread_api_->getType(F2);
  bool op1_is_write =
      (t1 == ThreadAPI::TD_MPI_PUT || t1 == ThreadAPI::TD_MPI_ACCUMULATE);
  bool op2_is_write =
      (t2 == ThreadAPI::TD_MPI_PUT || t2 == ThreadAPI::TD_MPI_ACCUMULATE);

  if (!op1_is_write && !op2_is_write)
    return false;

  if (op1.sync_model == SyncModel::NONE || op2.sync_model == SyncModel::NONE) {
    return true;
  }
  if (op1.sync_model != op2.sync_model) {
    return true;
  }
  if (op1.local_completion_only || op2.local_completion_only) {
    return true;
  }
  if (op1.epoch_id != 0 && op1.epoch_id == op2.epoch_id) {
    return false;
  }
  return true;
}

std::vector<MPIRMAAnalysis::RMAOperation>
MPIRMAAnalysis::findUnsynchronizedRMAOps() const {
  std::vector<RMAOperation> unsync;
  for (const RMAOperation &op : rma_operations_) {
    if (op.sync_model == SyncModel::NONE) {
      unsync.push_back(op);
    }
  }
  return unsync;
}

std::vector<
    std::pair<MPIRMAAnalysis::RMAOperation, MPIRMAAnalysis::RMAOperation>>
MPIRMAAnalysis::findRMARaces() const {
  std::vector<std::pair<RMAOperation, RMAOperation>> races;

  for (size_t i = 0; i < rma_operations_.size(); ++i) {
    for (size_t j = i + 1; j < rma_operations_.size(); ++j) {
      if (areRMAOpsConflicting(rma_operations_[i], rma_operations_[j])) {
        races.emplace_back(rma_operations_[i], rma_operations_[j]);
      }
    }
  }

  return races;
}

std::vector<WindowID> MPIRMAAnalysis::findLeakedWindows() const {
  std::vector<WindowID> leaked;
  for (const auto &pair : windows_) {
    if (!pair.second.free_inst) {
      leaked.push_back(pair.first);
    }
  }
  return leaked;
}

} // namespace mpi
