#pragma once

#include <string>

namespace lotus::concurrency::mpi {

// Canonical, fine-grained semantic operation kinds for MPI programs.
//
// These mirror the TD_MPI_* values in ThreadAPI.h but are independent of
// ThreadAPI::TD_TYPE, so MPI semantics can be reasoned about without depending
// on the generic thread-API enum. The mapping between the two is implemented
// in MPISemanticOp.cpp (which may include ThreadAPI.h).
enum class MPIOpKind {
  // MPI Session Management (MPI-4.0)
  SessionInit,             ///< MPI_Session_init
  SessionFinalize,         ///< MPI_Session_finalize
  SessionGetInfo,          ///< MPI_Session_get_info
  SessionGetNumErrcodes,   ///< MPI_Session_get_num_errcodes
  SessionGetErrhandler,    ///< MPI_Session_get_errhandler
  SessionSetErrhandler,    ///< MPI_Session_set_errhandler

  // MPI Error Handling
  ErrhandlerCreate,        ///< MPI_Errhandler_create
  ErrhandlerFree,          ///< MPI_Errhandler_free
  CommGetErrhandler,       ///< MPI_Comm_get_errhandler
  CommSetErrhandler,       ///< MPI_Comm_set_errhandler
  CommCallErrhandler,      ///< MPI_Comm_call_errhandler
  WinGetErrhandler,        ///< MPI_Win_get_errhandler
  WinSetErrhandler,        ///< MPI_Win_set_errhandler
  FileGetErrhandler,       ///< MPI_File_get_errhandler
  FileSetErrhandler,       ///< MPI_File_set_errhandler
  ErrorClass,              ///< MPI_Error_class
  ErrorString,             ///< MPI_Error_string

  // MPI Info Management
  InfoCreate,              ///< MPI_Info_create
  InfoDup,                 ///< MPI_Info_dup
  InfoFree,                ///< MPI_Info_free
  InfoGet,                 ///< MPI_Info_get
  InfoGetValuelen,         ///< MPI_Info_get_valuelen
  InfoGetNkeys,            ///< MPI_Info_get_nkeys
  InfoGetNthkey,           ///< MPI_Info_get_nthkey
  InfoGetKeyval,           ///< MPI_Info_get_keyval
  InfoSet,                 ///< MPI_Info_set
  InfoDelete,              ///< MPI_Info_delete
  InfoC2F,                 ///< MPI_Info_c2f
  InfoCreateEnv,           ///< MPI_Info_create_env
  InfoFreeEnv,             ///< MPI_Info_free_env

  // MPI Buffer Query Operations
  GetCount,                ///< MPI_Get_count
  GetElements,             ///< MPI_Get_elements
  GetElementsX,            ///< MPI_Get_elements_x
  StatusSize,              ///< MPI_Status_size
  StatusSetElements,       ///< MPI_Status_set_elements
  StatusSetElementsX,      ///< MPI_Status_set_elements_x

  // MPI Message Matching (MPI-3.0)
  Mprobe,                  ///< MPI_Mprobe
  Improbe,                 ///< MPI_Improbe
  Imrecv,                  ///< MPI_Imrecv
  Mrecv,                   ///< MPI_Mrecv

  // MPI Process Management
  Init,                    ///< MPI_Init, MPI_Init_thread
  Finalize,                ///< MPI_Finalize

  // MPI Point-to-Point (blocking = synchronization point)
  Send,                    ///< MPI_Send, MPI_Ssend, MPI_Bsend, MPI_Rsend
  Recv,                    ///< MPI_Recv
  Sendrecv,                ///< MPI_Sendrecv, MPI_Sendrecv_replace
  Probe,                   ///< MPI_Probe

  // MPI Point-to-Point (non-blocking)
  Isend,                   ///< MPI_Isend, MPI_Issend, MPI_Ibsend, MPI_Irsend
  Irecv,                   ///< MPI_Irecv
  Iprobe,                  ///< MPI_Iprobe
  PersistentSendInit,      ///< MPI_Send_init
  PersistentRecvInit,      ///< MPI_Recv_init
  RequestStart,            ///< MPI_Start, MPI_Startall

  // MPI Synchronization
  Wait,                    ///< MPI_Wait (join-like for non-blocking ops)
  Waitall,                 ///< MPI_Waitall
  Waitany,                 ///< MPI_Waitany
  Waitsome,                ///< MPI_Waitsome
  Test,                    ///< MPI_Test
  Testall,                 ///< MPI_Testall
  Testany,                 ///< MPI_Testany
  Testsome,                ///< MPI_Testsome
  Barrier,                 ///< MPI_Barrier, MPI_Ibarrier

  // MPI Collectives (all are synchronization points)
  Bcast,                   ///< MPI_Bcast, MPI_Ibcast
  Scatter,                 ///< MPI_Scatter, MPI_Scatterv, MPI_I*
  Gather,                  ///< MPI_Gather, MPI_Gatherv, MPI_I*
  Allgather,               ///< MPI_Allgather, MPI_Allgatherv, MPI_I*
  Alltoall,                ///< MPI_Alltoall, MPI_Alltoallv, MPI_Alltoallw, MPI_I*
  Reduce,                  ///< MPI_Reduce, MPI_Ireduce
  Allreduce,               ///< MPI_Allreduce, MPI_Iallreduce
  ReduceScatter,           ///< MPI_Reduce_scatter, MPI_Reduce_scatter_block, MPI_I*
  Scan,                    ///< MPI_Scan, MPI_Exscan, MPI_I*

  // MPI One-Sided (RMA - Remote Memory Access)
  WinCreate,               ///< MPI_Win_create, MPI_Win_allocate, MPI_Win_create_dynamic
  WinFree,                 ///< MPI_Win_free
  Put,                     ///< MPI_Put, MPI_Rput (shared write)
  Get,                     ///< MPI_Get, MPI_Rget (shared read)
  Accumulate,              ///< MPI_Accumulate, MPI_Get_accumulate, MPI_Fetch_and_op, etc.

  // MPI RMA Synchronization - Active Target
  WinFence,                ///< MPI_Win_fence (barrier for RMA)

  // MPI RMA Synchronization - Passive Target
  WinLock,                 ///< MPI_Win_lock, MPI_Win_lock_all (RMA lock)
  WinUnlock,               ///< MPI_Win_unlock, MPI_Win_unlock_all (RMA unlock)
  WinFlush,                ///< MPI_Win_flush, MPI_Win_flush_all, MPI_Win_flush_local*
  WinSync,                 ///< MPI_Win_sync (memory consistency)

  // MPI RMA Synchronization - General Purpose (PSCW)
  WinPost,                 ///< MPI_Win_post (exposure epoch start)
  WinStart,                ///< MPI_Win_start (access epoch start)
  WinComplete,             ///< MPI_Win_complete (access epoch end)
  WinWait,                 ///< MPI_Win_wait (exposure epoch end)
  WinTest,                 ///< MPI_Win_test (test exposure epoch)

  // MPI Communicator Management
  CommDup,                 ///< MPI_Comm_dup, MPI_Comm_idup
  CommSplit,               ///< MPI_Comm_split, MPI_Comm_split_type
  CommCreate,              ///< MPI_Comm_create, MPI_Comm_create_group
  CommFree,                ///< MPI_Comm_free

  // MPI Request Management
  RequestFree,             ///< MPI_Request_free
  Cancel,                  ///< MPI_Cancel

  // MPI Datatype Management
  TypeContiguous,          ///< MPI_Type_contiguous
  TypeVector,              ///< MPI_Type_vector
  TypeHvector,             ///< MPI_Type_hvector
  TypeIndexed,             ///< MPI_Type_indexed
  TypeHindexed,            ///< MPI_Type_hindexed
  TypeStruct,              ///< MPI_Type_struct
  TypeCreateDlpack,        ///< MPI_Type_create_dlpack (MPI-4.1)
  TypeCreateSubarray,      ///< MPI_Type_create_subarray
  TypeCreateDarray,        ///< MPI_Type_create_darray
  TypeCreateResized,       ///< MPI_Type_create_resized
  TypeCreateHindexed,      ///< MPI_Type_create_hindexed (legacy)
  TypeCreateHvector,       ///< MPI_Type_create_hvector (legacy)
  TypeGetExtent,           ///< MPI_Type_get_extent
  TypeGetTrueExtent,       ///< MPI_Type_get_true_extent
  TypeSize,                ///< MPI_Type_size
  TypeCommit,              ///< MPI_Type_commit

  // MPI Process Topology (MPI-2.2+)
  CartCreate,              ///< MPI_Cart_create - Cartesian topology
  CartDimsCreate,          ///< MPI_Cart_dims_create - create dimension sizes
  CartGet,                 ///< MPI_Cart_get - get Cartesian topology info
  CartShift,               ///< MPI_Cart_shift - get shift source/dest
  CartCoords,              ///< MPI_Cart_coords - get coords from rank
  CartRank,                ///< MPI_Cart_rank - get rank from coords
  CartSub,                 ///< MPI_Cart_sub - create sub-grid
  DistGraphCreate,         ///< MPI_Dist_graph_create - distributed graph
  DistGraphCreateAdjacent, ///< MPI_Dist_graph_create_adjacent
  DistGraphNeighbors,      ///< MPI_Dist_graph_neighbors
  DistGraphNeighborsCount, ///< MPI_Dist_graph_neighbors_count
  GraphCreate,             ///< MPI_Graph_create - deprecated but still used
  GraphGet,                ///< MPI_Graph_get
  GraphNeighbors,          ///< MPI_Graph_neighbors
  GraphNeighborsCount,     ///< MPI_Graph_neighbors_count
  GraphDimsGet,            ///< MPI_Graphdims_get
  GraphMap,                ///< MPI_Graph_map

  Unknown
};

// Canonical string name for an MPI operation kind (e.g. "MPI_Send").
std::string mpiOpToString(MPIOpKind op);

// Map a ThreadAPI::TD_TYPE value to the canonical MPIOpKind.
// The TD_TYPE is passed as an int to avoid including ThreadAPI.h here; the
// mapping itself is implemented in MPISemanticOp.cpp.
// Returns MPIOpKind::Unknown for non-MPI thread-API types.
MPIOpKind mpiOpFromTDType(int td_type);

} // namespace lotus::concurrency::mpi