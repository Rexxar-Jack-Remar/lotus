#include "Analysis/Concurrency/MPI/MPISemantics.h"

#include "Analysis/Concurrency/MPI/MPISymbol.h"

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace mpi {

namespace {

using TD = ThreadAPI::TD_TYPE;

constexpr MPISemanticDescriptor
makeDesc(TD type, MPIOpKind kind, MPISemanticFamily family,
         int communicator_arg = -1, int request_arg = -1, int count_arg = -1,
         int datatype_arg = -1, int peer_rank_arg = -1, int tag_arg = -1,
         bool peer_rank_is_dest = false) {
  MPISemanticDescriptor descriptor;
  descriptor.type = type;
  descriptor.kind = kind;
  descriptor.family = family;
  descriptor.communicator_arg = communicator_arg;
  descriptor.request_arg = request_arg;
  descriptor.count_arg = count_arg;
  descriptor.datatype_arg = datatype_arg;
  descriptor.peer_rank_arg = peer_rank_arg;
  descriptor.tag_arg = tag_arg;
  descriptor.peer_rank_is_dest = peer_rank_is_dest;
  return descriptor;
}

constexpr MPISemanticDescriptor makeSendRecvDesc(TD type) {
  MPISemanticDescriptor descriptor =
      makeDesc(type, MPIOpKind::UNKNOWN, MPISemanticFamily::PointToPoint);
  descriptor.split_into_sendrecv = true;
  return descriptor;
}

constexpr MPISemanticDescriptor makeCollectiveDesc(TD type,
                                                   bool barrier_family) {
  MPISemanticDescriptor descriptor =
      makeDesc(type,
               barrier_family ? MPIOpKind::BARRIER_BLOCKING
                              : MPIOpKind::COLLECTIVE_BLOCKING,
               MPISemanticFamily::Collective, -1);
  descriptor.trait_driven_barrier_kind = barrier_family;
  descriptor.trait_driven_collective_kind = !barrier_family;
  descriptor.collective_nonblocking_comm_arg = -2;
  descriptor.collective_nonblocking_request_arg = -1;
  return descriptor;
}

constexpr MPISemanticDescriptor makeRMADataDesc(TD type) {
  MPISemanticDescriptor descriptor =
      makeDesc(type, MPIOpKind::RMA_DATA, MPISemanticFamily::RMAData);
  descriptor.count_arg = 1;
  descriptor.datatype_arg = 2;
  descriptor.target_rank_arg = 3;
  descriptor.target_disp_arg = 4;
  descriptor.window_arg = 7;
  return descriptor;
}

constexpr MPISemanticDescriptor makeRMAWindowFreeDesc() {
  MPISemanticDescriptor descriptor = makeDesc(
      TD::TD_MPI_WIN_FREE, MPIOpKind::RMA_WINDOW, MPISemanticFamily::RMAWindow);
  descriptor.window_arg = 0;
  return descriptor;
}

constexpr MPISemanticDescriptor kDescriptors[] = {
    makeDesc(TD::TD_MPI_SESSION_INIT, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_FINALIZE, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_GET_INFO, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_GET_NUM_ERRCODES, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_GET_ERRHANDLER, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_SET_ERRHANDLER, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_INIT, MPIOpKind::INIT, MPISemanticFamily::Lifecycle),
    makeDesc(TD::TD_MPI_FINALIZE, MPIOpKind::FINALIZE,
             MPISemanticFamily::Lifecycle),
    makeDesc(TD::TD_MPI_SEND, MPIOpKind::SEND_BLOCKING,
             MPISemanticFamily::PointToPoint, 5, -1, 1, 2, 3, 4, true),
    makeDesc(TD::TD_MPI_RECV, MPIOpKind::RECV_BLOCKING,
             MPISemanticFamily::PointToPoint, 5, -1, 1, 2, 3, 4, false),
    makeSendRecvDesc(TD::TD_MPI_SENDRECV),
    makeDesc(TD::TD_MPI_PROBE, MPIOpKind::PROBE_BLOCKING,
             MPISemanticFamily::Probe, 2, -1, -1, -1, 0, 1, false),
    makeDesc(TD::TD_MPI_ISEND, MPIOpKind::SEND_NONBLOCKING,
             MPISemanticFamily::PointToPoint, 5, 6, 1, 2, 3, 4, true),
    makeDesc(TD::TD_MPI_IRECV, MPIOpKind::RECV_NONBLOCKING,
             MPISemanticFamily::PointToPoint, 5, 6, 1, 2, 3, 4, false),
    makeDesc(TD::TD_MPI_IPROBE, MPIOpKind::PROBE_NONBLOCKING,
             MPISemanticFamily::Probe, 2, -1, -1, -1, 0, 1, false),
    makeDesc(TD::TD_MPI_WAIT, MPIOpKind::WAIT, MPISemanticFamily::Request, -1,
             0),
    makeDesc(TD::TD_MPI_WAITALL, MPIOpKind::WAIT, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_WAITANY, MPIOpKind::WAIT, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_WAITSOME, MPIOpKind::WAIT, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_TEST, MPIOpKind::TEST, MPISemanticFamily::Request, -1,
             0),
    makeDesc(TD::TD_MPI_TESTALL, MPIOpKind::TEST, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_TESTANY, MPIOpKind::TEST, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_TESTSOME, MPIOpKind::TEST, MPISemanticFamily::Request,
             -1, 1),
    makeCollectiveDesc(TD::TD_MPI_BARRIER, true),
    makeCollectiveDesc(TD::TD_MPI_BCAST, false),
    makeCollectiveDesc(TD::TD_MPI_SCATTER, false),
    makeCollectiveDesc(TD::TD_MPI_GATHER, false),
    makeCollectiveDesc(TD::TD_MPI_ALLGATHER, false),
    makeCollectiveDesc(TD::TD_MPI_ALLTOALL, false),
    makeCollectiveDesc(TD::TD_MPI_REDUCE, false),
    makeCollectiveDesc(TD::TD_MPI_ALLREDUCE, false),
    makeCollectiveDesc(TD::TD_MPI_REDUCE_SCATTER, false),
    makeCollectiveDesc(TD::TD_MPI_SCAN, false),
    makeDesc(TD::TD_MPI_WIN_CREATE, MPIOpKind::RMA_WINDOW,
             MPISemanticFamily::RMAWindow),
    makeRMAWindowFreeDesc(),
    makeRMADataDesc(TD::TD_MPI_PUT),
    makeRMADataDesc(TD::TD_MPI_GET),
    makeRMADataDesc(TD::TD_MPI_ACCUMULATE),
    makeDesc(TD::TD_MPI_WIN_FENCE, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_LOCK, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_UNLOCK, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_FLUSH, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_SYNC, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_POST, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_START, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_COMPLETE, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_WAIT, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_TEST, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_COMM_DUP, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_COMM_SPLIT, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_COMM_CREATE, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_COMM_FREE, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_PERSISTENT_SEND_INIT, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, 5, 6, 1, 2, 3, 4, true),
    makeDesc(TD::TD_MPI_PERSISTENT_RECV_INIT, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, 5, 6, 1, 2, 3, 4, false),
    makeDesc(TD::TD_MPI_REQUEST_START, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, -1, 1),
    makeDesc(TD::TD_MPI_REQUEST_FREE, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, -1, 0),
    makeDesc(TD::TD_MPI_CANCEL, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, -1, 0),
    makeDesc(TD::TD_MPI_GET_COUNT, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_GET_ELEMENTS, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_GET_ELEMENTS_X, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_STATUS_SIZE, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_STATUS_SET_ELEMENTS, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_STATUS_SET_ELEMENTS_X, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_MPROBE, MPIOpKind::PROBE_NONBLOCKING,
             MPISemanticFamily::Probe, 2, -1, -1, -1, 0, 1, false),
    makeDesc(TD::TD_MPI_IMPROBE, MPIOpKind::PROBE_NONBLOCKING,
             MPISemanticFamily::Probe, 2, -1, -1, -1, 0, 1, false),
    makeDesc(TD::TD_MPI_IMRECV, MPIOpKind::PROBE_NONBLOCKING,
             MPISemanticFamily::Probe),
    makeDesc(TD::TD_MPI_MRECV, MPIOpKind::PROBE_NONBLOCKING,
             MPISemanticFamily::Probe),
    makeDesc(TD::TD_MPI_TYPE_CONTIGUOUS, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_VECTOR, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_HVECTOR, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_INDEXED, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_HINDEXED, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_STRUCT, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_DLPACK, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_SUBARRAY, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_DARRAY, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_RESIZED, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_HINDEXED, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_HVECTOR, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_GET_EXTENT, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_GET_TRUE_EXTENT, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_SIZE, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_COMMIT, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_CART_CREATE, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_DIMS_CREATE, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_GET, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_SHIFT, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_COORDS, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_RANK, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_SUB, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_DIST_GRAPH_CREATE, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_DIST_GRAPH_CREATE_ADJACENT, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_DIST_GRAPH_NEIGHBORS, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_CREATE, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_GET, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_NEIGHBORS, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_NEIGHBORS_COUNT, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_DIMS_GET, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_MAP, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
};

} // namespace

const MPISemanticDescriptor *lookupMPISemantic(ThreadAPI::TD_TYPE type) {
  for (const MPISemanticDescriptor &descriptor : kDescriptors) {
    if (descriptor.type == type) {
      return &descriptor;
    }
  }
  return nullptr;
}

MPIEffect buildMPIEffect(const llvm::Instruction *inst, ThreadAPI *api) {
  MPIEffect effect;
  if (!inst || !api) {
    return effect;
  }

  const auto *cb = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!cb) {
    return effect;
  }

  const llvm::Function *callee = cb->getCalledFunction();
  if (!callee) {
    return effect;
  }

  MPISymbolNormalization normalization = normalizeMPISymbol(callee->getName());
  effect.confidence = normalization.confidence;
  effect.semantic_tag = api->getSemanticTag(callee);
  effect.type = api->getType(callee);
  effect.descriptor = lookupMPISemantic(effect.type);
  if (!effect.descriptor) {
    return effect;
  }

  effect.family = effect.descriptor->family;
  effect.kind = effect.descriptor->kind;
  switch (effect.family) {
  case MPISemanticFamily::Lifecycle:
    effect.effect_kind = MPIEffectKind::Lifecycle;
    break;
  case MPISemanticFamily::Session:
    effect.effect_kind = MPIEffectKind::Session;
    break;
  case MPISemanticFamily::PointToPoint:
    effect.effect_kind = MPIEffectKind::PointToPoint;
    break;
  case MPISemanticFamily::Probe:
    effect.effect_kind = MPIEffectKind::Probe;
    break;
  case MPISemanticFamily::Request:
    effect.effect_kind = MPIEffectKind::Request;
    break;
  case MPISemanticFamily::Collective:
    effect.effect_kind = MPIEffectKind::Collective;
    break;
  case MPISemanticFamily::Communicator:
    effect.effect_kind = MPIEffectKind::Communicator;
    break;
  case MPISemanticFamily::RMAWindow:
    effect.effect_kind = MPIEffectKind::RMAWindow;
    break;
  case MPISemanticFamily::RMAData:
    effect.effect_kind = MPIEffectKind::RMAData;
    break;
  case MPISemanticFamily::RMASync:
    effect.effect_kind = MPIEffectKind::RMASync;
    break;
  case MPISemanticFamily::Datatype:
    effect.effect_kind = MPIEffectKind::Datatype;
    break;
  case MPISemanticFamily::Unknown:
    effect.effect_kind = MPIEffectKind::Unknown;
    break;
  }

  return effect;
}

} // namespace mpi
