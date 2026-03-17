#pragma once

#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/MPI/MPIOperation.h"

#include <map>
#include <vector>

namespace mpi {

enum class MPIEventKind {
  Lifecycle,
  PointToPoint,
  Collective,
  Request,
  Communicator,
  RMA,
  Datatype,
  Session,
  Unknown
};

enum class MPIRequestActionKind {
  None,
  IssueNonBlocking,
  CreatePersistent,
  ActivatePersistent,
  Observe,
  CompleteMust,
  CompleteMay,
  Cancel,
  Free
};

enum class MPIMatchProofKind { NoMatch, MustMatch, MayMatch, Unknown };

enum class MPIRMASyncModel { None, Fence, LockUnlock, PSCW };

enum class MPIRMAEpochCompletionKind { None, LocalOnly, RemoteGuaranteed };

enum class MPIRMAEpochProofKind { Must, May, Unknown };

struct MPICollectiveScope {
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t protocol_class_id = 0;

  bool operator<(const MPICollectiveScope &other) const {
    if (communicator_class_id != other.communicator_class_id) {
      return communicator_class_id < other.communicator_class_id;
    }
    if (communicator_subgroup_id != other.communicator_subgroup_id) {
      return communicator_subgroup_id < other.communicator_subgroup_id;
    }
    return protocol_class_id < other.protocol_class_id;
  }
};

struct MPICollectiveEventState {
  MPICollectiveScope scope;
  ThreadAPI::TD_TYPE type = ThreadAPI::TD_DUMMY;
  ProtocolReachability reachability = ProtocolReachability::Unknown;
  size_t protocol_slot = 0;
  int root_rank = -1;
  int count = -1;
  int recv_count = -1;
  int datatype = -1;
  int recv_datatype = -1;
  int reduction_op = -1;
  bool in_place = false;
};

struct MPIRequestTransition {
  MPIRequestActionKind action = MPIRequestActionKind::None;
  MPIRequestState from_state = MPIRequestState::Unknown;
  MPIRequestState to_state = MPIRequestState::Unknown;
  const llvm::Instruction *inst = nullptr;
};

struct MPIRequestEventState {
  MPIRequestActionKind action = MPIRequestActionKind::None;
  std::vector<RequestID> requests;
  std::vector<int> completed_indices;
  bool completion_flag_known = false;
  bool completion_flag = false;
  bool outcount_known = false;
  int outcount = 0;
};

struct MPIRequestStateSummary {
  RequestID request = nullptr;
  const llvm::Instruction *origin_inst = nullptr;
  const llvm::Instruction *activation_inst = nullptr;
  const llvm::Instruction *last_transition_inst = nullptr;
  MPIRequestState state = MPIRequestState::Unknown;
  bool is_persistent = false;
  bool is_collective = false;
  int peer_rank = -1;
  int tag = -1;
  CommunicatorID communicator = nullptr;
  std::vector<MPIRequestTransition> history;
};

struct MPIPointToPointEventState {
  bool is_send = false;
  bool is_recv = false;
  bool is_probe = false;
  int local_rank = -1;
  int peer_rank = -1;
  int peer_rank_min = -1;
  int peer_rank_max = -1;
  int tag = -1;
  size_t communicator_class_id = 0;
  ProtocolReachability reachability = ProtocolReachability::Unknown;
};

struct MPIPointToPointObligation {
  size_t lhs_operation_index = 0;
  size_t rhs_operation_index = 0;
  const llvm::Instruction *lhs_inst = nullptr;
  const llvm::Instruction *rhs_inst = nullptr;
  size_t communicator_class_id = 0;
  int send_rank = -1;
  int recv_rank = -1;
  int tag = -1;
  MPIMatchProofKind proof = MPIMatchProofKind::Unknown;
  concurrency::Relation relation;
};

struct MPIRMAEventState {
  WindowID window = nullptr;
  bool is_window_lifecycle = false;
  bool is_data_operation = false;
  bool is_sync_operation = false;
  int target_rank = -1;
  int target_rank_min = -1;
  int target_rank_max = -1;
  int64_t target_disp = -1;
  int64_t byte_length = -1;
  RMAEpochKind epoch_kind = RMAEpochKind::None;
  MPIRMASyncModel sync_model = MPIRMASyncModel::None;
  size_t epoch_id = 0;
  bool lock_all = false;
  bool flush_completed = false;
  bool local_completion_only = false;
  bool exposure_epoch_observed = false;
  MPIRMAEpochCompletionKind epoch_completion = MPIRMAEpochCompletionKind::None;
  MPIRMAEpochProofKind epoch_proof = MPIRMAEpochProofKind::Unknown;
  const llvm::Instruction *sync_start = nullptr;
  const llvm::Instruction *sync_end = nullptr;
};

struct MPIEvent {
  size_t operation_index = 0;
  const llvm::Instruction *inst = nullptr;
  const MPIOperation *operation = nullptr;
  MPIEventKind kind = MPIEventKind::Unknown;
  bool has_collective_semantics = false;
  bool has_request_semantics = false;
  bool has_point_to_point_semantics = false;
  bool has_rma_semantics = false;
  concurrency::Relation relation;
  MPICollectiveEventState collective;
  MPIRequestEventState request;
  MPIPointToPointEventState point_to_point;
  MPIRMAEventState rma;
};

inline int requestStatePriority(MPIRequestState state) {
  switch (state) {
  case MPIRequestState::Created:
    return 0;
  case MPIRequestState::Active:
    return 1;
  case MPIRequestState::Pending:
    return 2;
  case MPIRequestState::MayComplete:
    return 3;
  case MPIRequestState::MustComplete:
    return 4;
  case MPIRequestState::Terminal:
    return 5;
  case MPIRequestState::Consumed:
    return 6;
  case MPIRequestState::Freed:
    return 7;
  case MPIRequestState::Escaped:
    return 8;
  case MPIRequestState::Unknown:
    return 9;
  }
  return 0;
}

inline MPIRequestState joinRequestState(MPIRequestState lhs,
                                        MPIRequestState rhs) {
  return requestStatePriority(lhs) >= requestStatePriority(rhs) ? lhs : rhs;
}

} // namespace mpi
