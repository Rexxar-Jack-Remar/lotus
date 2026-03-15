#pragma once

#include "Analysis/Concurrency/MPI/MPINormalization.h"
#include "Analysis/Concurrency/MPI/MPIOperation.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <string>

namespace mpi {

enum class MPIEffectKind {
  Lifecycle,
  Session,
  PointToPoint,
  Probe,
  Request,
  Collective,
  Communicator,
  RMAWindow,
  RMAData,
  RMASync,
  Datatype,
  Unknown
};

enum class MPISemanticFamily {
  Lifecycle,
  Session,
  PointToPoint,
  Probe,
  Request,
  Collective,
  Communicator,
  RMAWindow,
  RMAData,
  RMASync,
  Datatype,
  Unknown
};

struct MPISemanticDescriptor {
  ThreadAPI::TD_TYPE type = ThreadAPI::TD_DUMMY;
  MPIOpKind kind = MPIOpKind::UNKNOWN;
  MPISemanticFamily family = MPISemanticFamily::Unknown;
  bool trait_driven_barrier_kind = false;
  bool trait_driven_collective_kind = false;
  bool split_into_sendrecv = false;

  // Signed argument indices: non-negative are absolute, negative are from end
  // (-1 = last argument).
  int communicator_arg = -1;
  int request_arg = -1;
  int count_arg = -1;
  int datatype_arg = -1;
  int peer_rank_arg = -1;
  int tag_arg = -1;
  int window_arg = -1;
  int target_rank_arg = -1;
  int target_disp_arg = -1;
  bool peer_rank_is_dest = false;

  int collective_nonblocking_comm_arg = -1;
  int collective_nonblocking_request_arg = -1;
};

struct MPIEffect {
  MPIEffectKind effect_kind = MPIEffectKind::Unknown;
  ThreadAPI::TD_TYPE type = ThreadAPI::TD_DUMMY;
  MPIOpKind kind = MPIOpKind::UNKNOWN;
  MPISemanticFamily family = MPISemanticFamily::Unknown;
  NormalizationConfidence confidence =
      NormalizationConfidence::UnknownVendorInternal;
  std::string semantic_tag;
  const MPISemanticDescriptor *descriptor = nullptr;
};

const MPISemanticDescriptor *lookupMPISemantic(ThreadAPI::TD_TYPE type);
MPIEffect buildMPIEffect(const llvm::Instruction *inst, ThreadAPI *api);

} // namespace mpi
