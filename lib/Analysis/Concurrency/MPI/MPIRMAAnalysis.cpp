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

bool isLockAllOperation(const MPIOperation &op) {
  return op.td_type == ThreadAPI::TD_MPI_WIN_LOCK && op.target_rank < 0;
}

} // namespace

void MPIRMAAnalysis::annotateOperationsInMachine(
    EpochMachine &machine, const Instruction *end_inst,
    concurrency::ProofStrength proof, StringRef reason,
    bool close_epoch) const {
  for (size_t idx : machine.op_indices) {
    auto &rma_op = const_cast<RMAOperation &>(rma_operations_[idx]);
    rma_op.sync_model = machine.model;
    rma_op.sync_start = machine.start;
    rma_op.sync_end = end_inst;
    rma_op.epoch_id = machine.epoch_id;
    rma_op.local_completion_only = machine.local_completion_only;
    rma_op.flush_completed = machine.remote_completion_observed;
    rma_op.exposure_epoch_observed = machine.exposure_epoch_observed;
    rma_op.synchronization_proof = proof;
    rma_op.relation.kind = concurrency::RelationKind::SameSynchronizationEpoch;
    rma_op.relation.proof = proof;
    rma_op.relation.reason = reason.str();
  }

  if (close_epoch) {
    machine.state = EpochState::Idle;
    machine.model = SyncModel::NONE;
    machine.start = nullptr;
    machine.epoch_id = 0;
    machine.local_completion_only = false;
    machine.remote_completion_observed = false;
    machine.exposure_epoch_observed = false;
    machine.op_indices.clear();
  }
}

bool MPIRMAAnalysis::transitionEpochMachine(EpochMachine &machine,
                                            const MPIOperation &op,
                                            size_t next_epoch_id) const {
  switch (op.td_type) {
  case ThreadAPI::TD_MPI_WIN_FENCE:
    if (machine.state == EpochState::FenceOpen) {
      annotateOperationsInMachine(machine, op.inst,
                                  concurrency::ProofStrength::Must,
                                  "mpi_rma_fence_epoch", true);
      return true;
    }
    if (machine.state == EpochState::Idle) {
      machine.state = EpochState::FenceOpen;
      machine.model = SyncModel::FENCE;
      machine.start = op.inst;
      machine.epoch_id = next_epoch_id;
      return true;
    }
    return false;
  case ThreadAPI::TD_MPI_WIN_LOCK:
    if (machine.state != EpochState::Idle) {
      return false;
    }
    machine.state = isLockAllOperation(op) ? EpochState::LockAllOpen
                                           : EpochState::LockOpen;
    machine.model = SyncModel::LOCK_UNLOCK;
    machine.start = op.inst;
    machine.epoch_id = next_epoch_id;
    return true;
  case ThreadAPI::TD_MPI_WIN_UNLOCK:
    if (machine.state == EpochState::LockOpen) {
      annotateOperationsInMachine(machine, op.inst,
                                  concurrency::ProofStrength::Must,
                                  "mpi_rma_lock_epoch", true);
      return true;
    }
    if (machine.state == EpochState::LockAllOpen) {
      annotateOperationsInMachine(machine, op.inst,
                                  concurrency::ProofStrength::Must,
                                  "mpi_rma_lock_all_epoch", true);
      return true;
    }
    return false;
  case ThreadAPI::TD_MPI_WIN_FLUSH:
  case ThreadAPI::TD_MPI_WIN_SYNC:
    if (machine.state != EpochState::LockOpen &&
        machine.state != EpochState::LockAllOpen) {
      return false;
    }
    machine.remote_completion_observed =
        op.td_type != ThreadAPI::TD_MPI_WIN_FLUSH || !op.rma_local_completion_only;
    machine.local_completion_only = op.rma_local_completion_only;
    annotateOperationsInMachine(
        machine, op.inst,
        op.rma_local_completion_only ? concurrency::ProofStrength::May
                                     : concurrency::ProofStrength::Must,
        op.rma_local_completion_only ? "mpi_rma_flush_local_completion"
                                     : "mpi_rma_flush_completion",
        false);
    return true;
  case ThreadAPI::TD_MPI_WIN_START:
    if (machine.state != EpochState::Idle) {
      return false;
    }
    machine.state = EpochState::PSCWAccessOpen;
    machine.model = SyncModel::PSCW;
    machine.start = op.inst;
    machine.epoch_id = next_epoch_id;
    return true;
  case ThreadAPI::TD_MPI_WIN_COMPLETE:
    if (machine.state != EpochState::PSCWAccessOpen) {
      return false;
    }
    annotateOperationsInMachine(machine, op.inst,
                                concurrency::ProofStrength::Must,
                                "mpi_rma_pscw_access_epoch", true);
    return true;
  case ThreadAPI::TD_MPI_WIN_POST:
    if (machine.state != EpochState::Idle) {
      return false;
    }
    machine.state = EpochState::PSCWExposureOpen;
    machine.model = SyncModel::PSCW;
    machine.start = op.inst;
    machine.epoch_id = next_epoch_id;
    return true;
  case ThreadAPI::TD_MPI_WIN_WAIT:
    if (machine.state != EpochState::PSCWExposureOpen) {
      return false;
    }
    machine.exposure_epoch_observed = true;
    annotateOperationsInMachine(machine, op.inst,
                                concurrency::ProofStrength::Must,
                                "mpi_rma_pscw_exposure_epoch", true);
    return true;
  case ThreadAPI::TD_MPI_WIN_TEST:
    if (machine.state == EpochState::PSCWExposureOpen) {
      machine.exposure_epoch_observed = true;
      annotateOperationsInMachine(machine, op.inst,
                                  concurrency::ProofStrength::May,
                                  "mpi_rma_pscw_exposure_test", false);
      return true;
    }
    return false;
  default:
    return false;
  }
}

void MPIRMAAnalysis::analyzeRMA() {
  windows_.clear();
  rma_operations_.clear();
  invalid_epoch_transitions_.clear();
  use_after_free_windows_.clear();
  double_window_free_.clear();

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
          if (it->second.free_inst) {
            double_window_free_.push_back(op.inst);
          }
          it->second.free_inst = op.inst;
        }
      }
    }
  }

  std::map<std::pair<const Function *, WindowID>, EpochMachine> epoch_machines;
  size_t next_epoch_id = 1;

  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if ((op.kind == MPIOpKind::RMA_DATA || op.kind == MPIOpKind::RMA_SYNC) &&
        op.window) {
      auto win_it = windows_.find(op.window);
      if (win_it != windows_.end() && win_it->second.free_inst &&
          win_it->second.free_inst != op.inst) {
        use_after_free_windows_.push_back(op.inst);
      }
    }

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
      auto &machine = epoch_machines[key];
      if (machine.state != EpochState::Idle) {
        machine.op_indices.push_back(op_index);
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
      auto &machine = epoch_machines[key];
      if (transitionEpochMachine(machine, op, next_epoch_id)) {
        if (machine.epoch_id == next_epoch_id) {
          ++next_epoch_id;
        }
      } else {
        invalid_epoch_transitions_.push_back(op.inst);
        for (size_t idx : machine.op_indices) {
          rma_operations_[idx].relation.kind =
              concurrency::RelationKind::UnknownDueToModelGap;
          rma_operations_[idx].relation.proof =
              concurrency::ProofStrength::May;
          rma_operations_[idx].relation.reason =
              "mpi_rma_invalid_epoch_transition";
        }
        machine = EpochMachine{};
      }
    }
  }
}

std::vector<const Instruction *>
MPIRMAAnalysis::findInvalidEpochTransitions() const {
  return invalid_epoch_transitions_;
}

std::vector<const Instruction *> MPIRMAAnalysis::findUseAfterFreeWindows() const {
  return use_after_free_windows_;
}

std::vector<const Instruction *> MPIRMAAnalysis::findDoubleWindowFree() const {
  return double_window_free_;
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
