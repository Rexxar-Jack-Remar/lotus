/**
 * @file MPIOperation.h
 * @brief MPI Operation Types and Structures
 *
 * This file defines the core types, enums, and structures used for
 * MPI program analysis.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_OPERATION_H
#define MPI_OPERATION_H

#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace mpi {

// ============================================================================
// Type Definitions
// ============================================================================

using ProcessID = int;
using CommunicatorID = const llvm::Value *;
using RequestID = const llvm::Value *;
using WindowID = const llvm::Value *;

// ============================================================================
// Enumerations
// ============================================================================

enum class MPICommunicationMatch { NoMatch, MustMatch, MayMatch, Unknown };

enum class RequestCompletionState {
  Pending,
  MayComplete,
  MustComplete,
  Terminal
};

enum class ProtocolReachability { AllRanks, SomeRanks, Unknown };

enum class RMAEpochKind { None, Access, Exposure };

enum class MPIOpKind {
  INIT,
  FINALIZE,
  SESSION,
  SEND_BLOCKING,
  RECV_BLOCKING,
  PROBE_BLOCKING,
  SEND_NONBLOCKING,
  RECV_NONBLOCKING,
  PROBE_NONBLOCKING,
  WAIT,
  TEST,
  BARRIER_BLOCKING,
  BARRIER_NONBLOCKING,
  COLLECTIVE_BLOCKING,
  COLLECTIVE_NONBLOCKING,
  RMA_WINDOW,
  RMA_DATA,
  RMA_SYNC,
  COMM_MANAGEMENT,
  INTERCOMM_CREATION,
  REQUEST_MANAGEMENT,
  DATATYPE_CREATE,
  UNKNOWN
};

// ============================================================================
// MPI Operation Structure
// ============================================================================

struct MPIOperation {
  const llvm::Instruction *inst;
  MPIOpKind kind;
  ThreadAPI::TD_TYPE td_type;

  const llvm::Function *function;
  CommunicatorID communicator = nullptr;
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t collective_protocol_class_id = 0;
  size_t protocol_sequence_id = 0;
  ProtocolReachability protocol_reachability = ProtocolReachability::Unknown;
  concurrency::Relation semantic_relation;
  MPI::RankExpr process_rank;
  std::string rank_path_summary;
  bool is_intercommunicator = false;

  int source_rank = -1;
  int dest_rank = -1;
  int tag = -1;
  int source_rank_min = -1;
  int source_rank_max = -1;
  int dest_rank_min = -1;
  int dest_rank_max = -1;

  RequestID request = nullptr;
  const llvm::Instruction *completion_inst = nullptr;
  RequestCompletionState request_state = RequestCompletionState::Pending;

  WindowID window = nullptr;
  int target_rank = -1;
  int target_rank_min = -1;
  int target_rank_max = -1;
  int64_t target_disp = -1;
  int64_t byte_length = -1;
  bool rma_local_completion_only = false;

  const llvm::Value *datatype = nullptr;
  int64_t datatype_size = -1;
  bool is_derived_datatype = false;

  RMAEpochKind rma_epoch_kind = RMAEpochKind::None;
  concurrency::ProofStrength synchronization_proof =
      concurrency::ProofStrength::Unknown;

  MPIOperation() = default;
  MPIOperation(const llvm::Instruction *i, MPIOpKind k, ThreadAPI::TD_TYPE t)
      : inst(i), kind(k), td_type(t), function(i->getFunction()) {}
};

} // namespace mpi

#endif // MPI_OPERATION_H
