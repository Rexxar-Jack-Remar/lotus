#include "Analysis/Concurrency/MPI/MPISemantics.h"

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

} // namespace mpi
