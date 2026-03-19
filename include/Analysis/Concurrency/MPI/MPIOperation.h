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
#include "Analysis/Concurrency/MPI/MPINormalization.h"
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

using ProcessID = int;
using CommunicatorID = const llvm::Value *;
using RequestID = const llvm::Value *;
using WindowID = const llvm::Value *;

enum class MPICommunicationMatch { NoMatch, MustMatch, MayMatch, Unknown };

enum class MPIRequestState {
  Unbound,
  PersistentTemplate,
  InactivePersistent,
  Active,
  MayComplete,
  MustComplete,
  Canceled,
  Freed,
  Escaped,
  Unknown,
  // Compatibility aliases for older callers/tests.
  Created = PersistentTemplate,
  Pending = Active,
  Terminal = Canceled,
  Consumed = MustComplete
};

using RequestCompletionState = MPIRequestState;

enum class ProtocolReachability { AllRanks, SomeRanks, Unknown };

enum class RMAEpochKind { None, Access, Exposure };

enum class MPISendMode { Standard, Synchronous, Buffered, Ready, Unknown };

enum class MPIBlockingMode {
  Blocking,
  NonBlocking,
  Completion,
  LocalCompletion,
  Unknown
};

enum class MPIRequestArity { None, Single, Array };

enum class MPICollectiveVariant {
  Unknown,
  Barrier,
  Bcast,
  Gather,
  Gatherv,
  Scatter,
  Scatterv,
  Allgather,
  Allgatherv,
  Alltoall,
  Alltoallv,
  Alltoallw,
  Reduce,
  Allreduce,
  ReduceScatter,
  ReduceScatterBlock,
  Scan,
  Exscan,
  NeighborAllgather,
  NeighborAllgatherv,
  NeighborAlltoall,
  NeighborAlltoallv,
  NeighborAlltoallw,
  IntercommBcast
};

enum class MPICollectiveShape {
  Unknown,
  Barrier,
  Rooted,
  AllToAll,
  Reduction,
  Scan,
  Neighbor,
  Intercommunicator
};

enum class MPIRMAAccessKind { None, Put, Get, Accumulate, Atomic };

enum class MPIRMASyncKind {
  None,
  Fence,
  Lock,
  LockAll,
  Unlock,
  UnlockAll,
  Flush,
  FlushAll,
  FlushLocal,
  FlushLocalAll,
  Sync,
  PSCWPost,
  PSCWStart,
  PSCWComplete,
  PSCWWait,
  PSCWTest
};

enum class MPIRMACompletionStrength { None, Local, Remote };

enum class MPIModelGapDomain {
  None,
  Rank,
  ParticipantSet,
  Communicator,
  CollectiveProtocol,
  PointToPoint,
  RequestLifecycle,
  RMAEpoch,
  Completion,
  Unknown
};

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

struct MPIParticipantSet {
  CommunicatorID communicator = nullptr;
  bool unknown = true;
  bool universal = false;
  int min_rank = 0;
  int max_rank = -1;
  std::set<int> excluded_ranks;
  size_t predicate_class_id = 0;
  size_t participant_class_id = 0;

  static MPIParticipantSet fromPredicate(const MPI::MPIRankPredicate &predicate,
                                         size_t predicate_class,
                                         size_t participant_class) {
    MPIParticipantSet set;
    set.communicator = predicate.communicator;
    set.unknown = predicate.unknown;
    set.universal = predicate.universal;
    set.min_rank = predicate.min_rank;
    set.max_rank = predicate.max_rank;
    set.excluded_ranks = predicate.excluded_ranks;
    set.predicate_class_id = predicate_class;
    set.participant_class_id = participant_class;
    return set;
  }

  bool constrainsParticipants() const {
    if (unknown) {
      return false;
    }
    if (!universal) {
      return true;
    }
    return !excluded_ranks.empty() || max_rank >= 0;
  }

  bool contains(int rank) const {
    if (unknown) {
      return true;
    }
    if (rank < min_rank) {
      return false;
    }
    if (max_rank >= 0 && rank > max_rank) {
      return false;
    }
    return excluded_ranks.count(rank) == 0;
  }

  bool mayOverlap(const MPIParticipantSet &other) const {
    if (unknown || other.unknown) {
      return true;
    }
    const int lhs_upper = max_rank >= 0 ? max_rank : other.max_rank;
    const int rhs_upper = other.max_rank >= 0 ? other.max_rank : max_rank;
    const int overlap_min = std::max(min_rank, other.min_rank);
    const int overlap_max =
        std::min(lhs_upper >= 0 ? lhs_upper : overlap_min + 1024,
                 rhs_upper >= 0 ? rhs_upper : overlap_min + 1024);
    if (overlap_min > overlap_max) {
      return false;
    }
    for (int rank = overlap_min; rank <= overlap_max; ++rank) {
      if (contains(rank) && other.contains(rank)) {
        return true;
      }
    }
    return universal || other.universal;
  }

  bool mustEqual(const MPIParticipantSet &other) const {
    return communicator == other.communicator && unknown == other.unknown &&
           universal == other.universal && min_rank == other.min_rank &&
           max_rank == other.max_rank &&
           excluded_ranks == other.excluded_ranks;
  }

  std::string toKey() const {
    if (unknown) {
      return "participants:unknown";
    }
    std::string key = universal ? "participants:universal"
                                : "participants:bounded";
    key += ":min=" + std::to_string(min_rank);
    key += ":max=" + std::to_string(max_rank);
    key += ":predicate=" + std::to_string(predicate_class_id);
    key += ":participant=" + std::to_string(participant_class_id);
    if (!excluded_ranks.empty()) {
      key += ":exclude=";
      bool first = true;
      for (int rank : excluded_ranks) {
        if (!first) {
          key += ",";
        }
        first = false;
        key += std::to_string(rank);
      }
    }
    return key;
  }
};

struct MPIModelGap {
  MPIModelGapDomain domain = MPIModelGapDomain::None;
  const llvm::Instruction *inst = nullptr;
  CommunicatorID communicator = nullptr;
  size_t communicator_class_id = 0;
  size_t participant_class_id = 0;
  concurrency::Relation relation;
  std::string code;
  std::string detail;
};

struct MPIChannelObligation {
  size_t lhs_operation_index = 0;
  size_t rhs_operation_index = 0;
  size_t sender_operation_index = 0;
  size_t receiver_operation_index = 0;
  const llvm::Instruction *lhs_inst = nullptr;
  const llvm::Instruction *rhs_inst = nullptr;
  const llvm::Instruction *sender_inst = nullptr;
  const llvm::Instruction *receiver_inst = nullptr;
  size_t communicator_class_id = 0;
  MPIParticipantSet sender_set;
  MPIParticipantSet receiver_set;
  int tag = -1;
  int64_t send_datatype_size = -1;
  int64_t recv_datatype_size = -1;
  MPISendMode send_mode = MPISendMode::Unknown;
  RequestID request = nullptr;
  RequestID sender_request = nullptr;
  RequestID receiver_request = nullptr;
  bool send_is_blocking = false;
  bool recv_is_blocking = false;
  bool discharged = false;
  const llvm::Instruction *discharge_inst = nullptr;
  MPICommunicationMatch proof = MPICommunicationMatch::Unknown;
  concurrency::Relation relation;
};

struct CollectiveProtocolFrontier {
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t participant_class_id = 0;
  size_t protocol_class_id = 0;
  size_t frontier_id = 0;
  size_t frontier_position = 0;
  MPIParticipantSet participants;
  concurrency::Relation relation;
  std::vector<const llvm::Instruction *> transitions;
  std::vector<std::string> diagnostics;
};

struct RMASynchronizationFact {
  const llvm::Instruction *inst = nullptr;
  WindowID window = nullptr;
  size_t participant_class_id = 0;
  MPIParticipantSet participants;
  int target_rank = -1;
  int target_rank_min = -1;
  int target_rank_max = -1;
  int64_t target_disp = -1;
  int64_t byte_length = -1;
  MPIRMAAccessKind access_kind = MPIRMAAccessKind::None;
  MPIRMASyncKind sync_kind = MPIRMASyncKind::None;
  RMAEpochKind access_epoch_kind = RMAEpochKind::None;
  RMAEpochKind exposure_epoch_kind = RMAEpochKind::None;
  size_t epoch_id = 0;
  MPIRMACompletionStrength completion = MPIRMACompletionStrength::None;
  concurrency::Relation relation;
  std::string code;
};

struct MPIOperation {
  const llvm::Instruction *inst;
  MPIOpKind kind;
  ThreadAPI::TD_TYPE td_type;

  const llvm::Function *function;
  NormalizationConfidence normalization_confidence =
      NormalizationConfidence::UnknownVendorInternal;
  CommunicatorID communicator = nullptr;
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t predicate_class_id = 0;
  size_t participant_class_id = 0;
  size_t collective_protocol_class_id = 0;
  size_t protocol_sequence_id = 0;
  size_t channel_class_id = 0;
  ProtocolReachability protocol_reachability = ProtocolReachability::Unknown;
  concurrency::Relation semantic_relation;
  MPI::RankExpr process_rank;
  MPI::MPIRankPredicate rank_predicate;
  MPIParticipantSet participant_set;
  std::string rank_path_summary;
  bool is_intercommunicator = false;

  MPISendMode send_mode = MPISendMode::Unknown;
  MPIBlockingMode blocking_mode = MPIBlockingMode::Unknown;
  MPIRequestArity request_arity = MPIRequestArity::None;
  MPICollectiveVariant collective_variant = MPICollectiveVariant::Unknown;
  MPICollectiveShape collective_shape = MPICollectiveShape::Unknown;
  MPIRMAAccessKind rma_access_kind = MPIRMAAccessKind::None;
  MPIRMASyncKind rma_sync_kind = MPIRMASyncKind::None;

  int source_rank = -1;
  int dest_rank = -1;
  int tag = -1;
  int source_rank_min = -1;
  int source_rank_max = -1;
  int dest_rank_min = -1;
  int dest_rank_max = -1;

  RequestID request = nullptr;
  const llvm::Instruction *completion_inst = nullptr;
  MPIRequestState request_state = MPIRequestState::Unbound;

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
      : inst(i), kind(k), td_type(t), function(i ? i->getFunction() : nullptr) {}
};

} // namespace mpi

#endif // MPI_OPERATION_H
