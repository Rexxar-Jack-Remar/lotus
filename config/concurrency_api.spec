# Structured concurrency runtime metadata.
# Format:
#   SymbolOrPrefix TD_TYPE library=<name> semantic=<tag> [match=exact|prefix]
#
# Exact is the default. Prefix is useful for OpenMP and MPI runtime families
# whose symbol suffix varies by ABI/version.

# OpenMP exact matches
__kmpc_fork_call TD_FORK library=openmp semantic=fork
__kmpc_barrier TD_BAR_WAIT library=openmp semantic=barrier
__kmpc_critical TD_ACQUIRE library=openmp semantic=critical-enter
__kmpc_end_critical TD_RELEASE library=openmp semantic=critical-exit
omp_set_lock TD_ACQUIRE library=openmp semantic=lock
omp_unset_lock TD_RELEASE library=openmp semantic=unlock
omp_test_lock TD_TRY_ACQUIRE library=openmp semantic=try-lock
omp_init_lock TD_MUTEX_INI library=openmp semantic=init-lock
omp_destroy_lock TD_MUTEX_DESTROY library=openmp semantic=destroy-lock
omp_set_nest_lock TD_ACQUIRE library=openmp semantic=nest-lock
omp_unset_nest_lock TD_RELEASE library=openmp semantic=nest-unlock
omp_test_nest_lock TD_TRY_ACQUIRE library=openmp semantic=nest-try-lock
omp_init_nest_lock TD_MUTEX_INI library=openmp semantic=init-nest-lock
omp_destroy_nest_lock TD_MUTEX_DESTROY library=openmp semantic=destroy-nest-lock
__kmpc_omp_task TD_OMP_TASK library=openmp semantic=task traits=omp-task-op
__kmpc_omp_task_begin_if0 TD_OMP_TASK library=openmp semantic=task-inline traits=omp-task-op
__kmpc_omp_taskwait TD_OMP_TASKWAIT library=openmp semantic=taskwait traits=omp-task-op
__kmpc_omp_wait_deps TD_OMP_TASKWAIT_DEPS library=openmp semantic=taskwait-deps traits=omp-task-op
__kmpc_omp_taskwait_deps_51 TD_OMP_TASKWAIT_DEPS library=openmp semantic=taskwait-deps traits=omp-task-op
__kmpc_omp_taskyield TD_OMP_TASKYIELD library=openmp semantic=taskyield traits=omp-task-op
__kmpc_taskgroup TD_OMP_TASKGROUP_START library=openmp semantic=taskgroup-start traits=omp-task-op
__kmpc_end_taskgroup TD_OMP_TASKGROUP_END library=openmp semantic=taskgroup-end traits=omp-task-op
__kmpc_taskloop TD_OMP_TASKLOOP library=openmp semantic=taskloop traits=omp-task-op
__kmpc_taskloop_nowait TD_OMP_TASKLOOP library=openmp semantic=taskloop-nowait traits=omp-task-op
__kmpc_omp_task_complete TD_OMP_TASK_COMPLETE library=openmp semantic=task-complete traits=omp-task-op
__kmpc_omp_task_complete_if0 TD_OMP_TASK_COMPLETE library=openmp semantic=task-complete-inline traits=omp-task-op
__kmpc_single TD_OMP_SINGLE_START library=openmp semantic=single-start traits=omp-task-op
__kmpc_end_single TD_OMP_SINGLE_END library=openmp semantic=single-end traits=omp-task-op,barrier-wait-like
__kmpc_master TD_OMP_MASTER_START library=openmp semantic=master-start traits=omp-task-op
__kmpc_end_master TD_OMP_MASTER_END library=openmp semantic=master-end traits=omp-task-op
__kmpc_ordered TD_OMP_ORDERED_START library=openmp semantic=ordered-start traits=omp-task-op
__kmpc_end_ordered TD_OMP_ORDERED_END library=openmp semantic=ordered-end traits=omp-task-op
__kmpc_reduce TD_OMP_REDUCE_START library=openmp semantic=reduce-start traits=omp-task-op,barrier-wait-like
__kmpc_end_reduce TD_OMP_REDUCE_END library=openmp semantic=reduce-end traits=omp-task-op
__kmpc_reduce_nowait TD_OMP_REDUCE_NOWAIT_START library=openmp semantic=reduce-nowait-start traits=omp-task-op
__kmpc_end_reduce_nowait TD_OMP_REDUCE_NOWAIT_END library=openmp semantic=reduce-nowait-end traits=omp-task-op
__kmpc_for_static_fini TD_OMP_FOR_STATIC_FINI library=openmp semantic=for-static-fini traits=omp-task-op,barrier-wait-like
__kmpc_sections_init TD_OMP_SECTIONS_INIT library=openmp semantic=sections-init traits=omp-task-op
__kmpc_next_section TD_OMP_SECTIONS_NEXT library=openmp semantic=sections-next traits=omp-task-op
__kmpc_end_sections TD_OMP_SECTIONS_END library=openmp semantic=sections-end traits=omp-task-op,barrier-wait-like
__kmpc_atomic_start TD_OMP_ATOMIC_START library=openmp semantic=atomic-start
__kmpc_atomic_end TD_OMP_ATOMIC_END library=openmp semantic=atomic-end
__kmpc_flush TD_OMP_FLUSH library=openmp semantic=flush
__kmpc_cancel TD_OMP_CANCEL library=openmp semantic=cancel
__kmpc_cancellationpoint TD_OMP_CANCEL library=openmp semantic=cancellation-point
__tgt_target_data_begin TD_OMP_TARGET_DATA_BEGIN library=openmp semantic=target-data-begin traits=omp-target-op,omp-target-data-op
__tgt_target_data_end TD_OMP_TARGET_DATA_END library=openmp semantic=target-data-end traits=omp-target-op,omp-target-data-op
__tgt_target_data_update TD_OMP_TARGET_DATA_UPDATE library=openmp semantic=target-data-update traits=omp-target-op,omp-target-data-op
__tgt_target_data_begin_nowait TD_OMP_TARGET_DATA_BEGIN library=openmp semantic=target-data-begin-nowait traits=omp-target-op,omp-target-data-op
__tgt_target_data_end_nowait TD_OMP_TARGET_DATA_END library=openmp semantic=target-data-end-nowait traits=omp-target-op,omp-target-data-op
__tgt_target_data_update_nowait TD_OMP_TARGET_DATA_UPDATE library=openmp semantic=target-data-update-nowait traits=omp-target-op,omp-target-data-op
__tgt_target_enter_data TD_OMP_TARGET_DATA_BEGIN library=openmp semantic=target-enter-data traits=omp-target-op,omp-target-data-op
__tgt_target_exit_data TD_OMP_TARGET_DATA_END library=openmp semantic=target-exit-data traits=omp-target-op,omp-target-data-op
__tgt_target_update TD_OMP_TARGET_DATA_UPDATE library=openmp semantic=target-update traits=omp-target-op,omp-target-data-op

# OpenMP prefix matches
__kmpc_omp_task_with_deps TD_OMP_TASK_WITH_DEPS library=openmp semantic=task-with-deps traits=omp-task-op match=prefix
__kmpc_for_static_init TD_OMP_FOR_STATIC_INIT library=openmp semantic=for-static-init traits=omp-task-op match=prefix
__kmpc_dispatch_init TD_OMP_FOR_DISPATCH_INIT library=openmp semantic=dispatch-init traits=omp-task-op match=prefix
__kmpc_dispatch_next TD_OMP_FOR_DISPATCH_NEXT library=openmp semantic=dispatch-next traits=omp-task-op match=prefix
__kmpc_dispatch_fini TD_OMP_FOR_DISPATCH_FINI library=openmp semantic=dispatch-fini traits=omp-task-op,barrier-wait-like match=prefix
__tgt_target TD_OMP_TARGET library=openmp semantic=target traits=omp-target-op match=prefix

# OpenMP 5.0+ Teams and Distribute prefix matches
__kmpc_teams TD_OMP_TEAMS library=openmp semantic=teams match=prefix
__kmpc_distribute TD_OMP_DISTRIBUTE library=openmp semantic=distribute match=prefix
__kmpc_loop TD_OMP_LOOP_STATIC_INIT library=openmp semantic=loop-static-init match=prefix
__kmpc_affinity TD_OMP_AFFINITY library=openmp semantic=affinity match=prefix
__kmpc_scope TD_OMP_SCOPE_START library=openmp semantic=scope-start match=prefix
__kmpc_end_scope TD_OMP_SCOPE_END library=openmp semantic=scope-end match=prefix
__kmpc_taskloop_simd TD_OMP_TASKLOOP_SIMD library=openmp semantic=taskloop-simd match=prefix
__kmpc_taskloop_fini TD_OMP_TASKLOOP_FINI library=openmp semantic=taskloop-fini match=prefix
__kmpc_interop TD_OMP_INTEROP_INIT library=openmp semantic=interop-init match=prefix
__kmpc_interop_fini TD_OMP_INTEROP_FINI library=openmp semantic=interop-fini match=prefix
__kmpc_doacross TD_OMP_DOACROSS_INIT library=openmp semantic=doacross-init match=prefix

# MPI process management and point-to-point
MPI_Init TD_MPI_INIT library=mpi semantic=init
MPI_Init_thread TD_MPI_INIT library=mpi semantic=init-thread
MPI_Finalize TD_MPI_FINALIZE library=mpi semantic=finalize
MPI_Send TD_MPI_SEND library=mpi semantic=send
MPI_Ssend TD_MPI_SEND library=mpi semantic=ssend
MPI_Bsend TD_MPI_SEND library=mpi semantic=bsend
MPI_Rsend TD_MPI_SEND library=mpi semantic=rsend
MPI_Recv TD_MPI_RECV library=mpi semantic=recv
MPI_Sendrecv TD_MPI_SENDRECV library=mpi semantic=sendrecv
MPI_Sendrecv_replace TD_MPI_SENDRECV library=mpi semantic=sendrecv-replace
MPI_Probe TD_MPI_PROBE library=mpi semantic=probe
MPI_Isend TD_MPI_ISEND library=mpi semantic=isend
MPI_Issend TD_MPI_ISEND library=mpi semantic=issend
MPI_Ibsend TD_MPI_ISEND library=mpi semantic=ibsend
MPI_Irsend TD_MPI_ISEND library=mpi semantic=irsend
MPI_Irecv TD_MPI_IRECV library=mpi semantic=irecv
MPI_Iprobe TD_MPI_IPROBE library=mpi semantic=iprobe
MPI_Wait TD_MPI_WAIT library=mpi semantic=wait
MPI_Waitall TD_MPI_WAITALL library=mpi semantic=waitall
MPI_Waitany TD_MPI_WAITANY library=mpi semantic=waitany
MPI_Waitsome TD_MPI_WAITSOME library=mpi semantic=waitsome
MPI_Test TD_MPI_TEST library=mpi semantic=test
MPI_Testall TD_MPI_TESTALL library=mpi semantic=testall
MPI_Testany TD_MPI_TESTANY library=mpi semantic=testany
MPI_Testsome TD_MPI_TESTSOME library=mpi semantic=testsome

# MPI collectives
MPI_Barrier TD_MPI_BARRIER library=mpi semantic=barrier traits=mpi-collective,mpi-barrier-blocking,mpi-collective-blocking
MPI_Ibarrier TD_MPI_BARRIER library=mpi semantic=ibarrier traits=mpi-collective,mpi-barrier-nonblocking,mpi-collective-nonblocking
MPI_Bcast TD_MPI_BCAST library=mpi semantic=bcast traits=mpi-collective,mpi-collective-blocking
MPI_Ibcast TD_MPI_BCAST library=mpi semantic=ibcast traits=mpi-collective,mpi-collective-nonblocking
MPI_Scatter TD_MPI_SCATTER library=mpi semantic=scatter traits=mpi-collective,mpi-collective-blocking
MPI_Scatterv TD_MPI_SCATTER library=mpi semantic=scatterv traits=mpi-collective,mpi-collective-blocking
MPI_Iscatter TD_MPI_SCATTER library=mpi semantic=iscatter traits=mpi-collective,mpi-collective-nonblocking
MPI_Iscatterv TD_MPI_SCATTER library=mpi semantic=iscatterv traits=mpi-collective,mpi-collective-nonblocking
MPI_Gather TD_MPI_GATHER library=mpi semantic=gather traits=mpi-collective,mpi-collective-blocking
MPI_Gatherv TD_MPI_GATHER library=mpi semantic=gatherv traits=mpi-collective,mpi-collective-blocking
MPI_Igather TD_MPI_GATHER library=mpi semantic=igather traits=mpi-collective,mpi-collective-nonblocking
MPI_Igatherv TD_MPI_GATHER library=mpi semantic=igatherv traits=mpi-collective,mpi-collective-nonblocking
MPI_Allgather TD_MPI_ALLGATHER library=mpi semantic=allgather traits=mpi-collective,mpi-collective-blocking
MPI_Allgatherv TD_MPI_ALLGATHER library=mpi semantic=allgatherv traits=mpi-collective,mpi-collective-blocking
MPI_Iallgather TD_MPI_ALLGATHER library=mpi semantic=iallgather traits=mpi-collective,mpi-collective-nonblocking
MPI_Iallgatherv TD_MPI_ALLGATHER library=mpi semantic=iallgatherv traits=mpi-collective,mpi-collective-nonblocking
MPI_Alltoall TD_MPI_ALLTOALL library=mpi semantic=alltoall traits=mpi-collective,mpi-collective-blocking
MPI_Alltoallv TD_MPI_ALLTOALL library=mpi semantic=alltoallv traits=mpi-collective,mpi-collective-blocking
MPI_Alltoallw TD_MPI_ALLTOALL library=mpi semantic=alltoallw traits=mpi-collective,mpi-collective-blocking
MPI_Ialltoall TD_MPI_ALLTOALL library=mpi semantic=ialltoall traits=mpi-collective,mpi-collective-nonblocking
MPI_Ialltoallv TD_MPI_ALLTOALL library=mpi semantic=ialltoallv traits=mpi-collective,mpi-collective-nonblocking
MPI_Ialltoallw TD_MPI_ALLTOALL library=mpi semantic=ialltoallw traits=mpi-collective,mpi-collective-nonblocking
MPI_Reduce TD_MPI_REDUCE library=mpi semantic=reduce traits=mpi-collective,mpi-collective-blocking
MPI_Ireduce TD_MPI_REDUCE library=mpi semantic=ireduce traits=mpi-collective,mpi-collective-nonblocking
MPI_Allreduce TD_MPI_ALLREDUCE library=mpi semantic=allreduce traits=mpi-collective,mpi-collective-blocking
MPI_Iallreduce TD_MPI_ALLREDUCE library=mpi semantic=iallreduce traits=mpi-collective,mpi-collective-nonblocking
MPI_Reduce_scatter TD_MPI_REDUCE_SCATTER library=mpi semantic=reduce-scatter traits=mpi-collective,mpi-collective-blocking
MPI_Reduce_scatter_block TD_MPI_REDUCE_SCATTER library=mpi semantic=reduce-scatter-block traits=mpi-collective,mpi-collective-blocking
MPI_Ireduce_scatter TD_MPI_REDUCE_SCATTER library=mpi semantic=ireduce-scatter traits=mpi-collective,mpi-collective-nonblocking
MPI_Ireduce_scatter_block TD_MPI_REDUCE_SCATTER library=mpi semantic=ireduce-scatter-block traits=mpi-collective,mpi-collective-nonblocking
MPI_Scan TD_MPI_SCAN library=mpi semantic=scan traits=mpi-collective,mpi-collective-blocking
MPI_Iscan TD_MPI_SCAN library=mpi semantic=iscan traits=mpi-collective,mpi-collective-nonblocking
MPI_Exscan TD_MPI_SCAN library=mpi semantic=exscan traits=mpi-collective,mpi-collective-blocking
MPI_Iexscan TD_MPI_SCAN library=mpi semantic=iexscan traits=mpi-collective,mpi-collective-nonblocking

# MPI RMA and communicator management
MPI_Win_create TD_MPI_WIN_CREATE library=mpi semantic=win-create
MPI_Win_allocate TD_MPI_WIN_CREATE library=mpi semantic=win-allocate
MPI_Win_create_dynamic TD_MPI_WIN_CREATE library=mpi semantic=win-create-dynamic
MPI_Win_allocate_shared TD_MPI_WIN_CREATE library=mpi semantic=win-allocate-shared
MPI_Win_free TD_MPI_WIN_FREE library=mpi semantic=win-free
MPI_Put TD_MPI_PUT library=mpi semantic=put
MPI_Rput TD_MPI_PUT library=mpi semantic=rput
MPI_Get TD_MPI_GET library=mpi semantic=get
MPI_Rget TD_MPI_GET library=mpi semantic=rget
MPI_Accumulate TD_MPI_ACCUMULATE library=mpi semantic=accumulate
MPI_Raccumulate TD_MPI_ACCUMULATE library=mpi semantic=raccumulate
MPI_Get_accumulate TD_MPI_ACCUMULATE library=mpi semantic=get-accumulate
MPI_Rget_accumulate TD_MPI_ACCUMULATE library=mpi semantic=rget-accumulate
MPI_Fetch_and_op TD_MPI_ACCUMULATE library=mpi semantic=fetch-and-op
MPI_Compare_and_swap TD_MPI_ACCUMULATE library=mpi semantic=compare-and-swap
MPI_Win_fence TD_MPI_WIN_FENCE library=mpi semantic=win-fence
MPI_Win_lock TD_MPI_WIN_LOCK library=mpi semantic=win-lock
MPI_Win_lock_all TD_MPI_WIN_LOCK library=mpi semantic=win-lock-all
MPI_Win_unlock TD_MPI_WIN_UNLOCK library=mpi semantic=win-unlock
MPI_Win_unlock_all TD_MPI_WIN_UNLOCK library=mpi semantic=win-unlock-all
MPI_Win_flush TD_MPI_WIN_FLUSH library=mpi semantic=win-flush
MPI_Win_flush_all TD_MPI_WIN_FLUSH library=mpi semantic=win-flush-all
MPI_Win_flush_local TD_MPI_WIN_FLUSH library=mpi semantic=win-flush-local
MPI_Win_flush_local_all TD_MPI_WIN_FLUSH library=mpi semantic=win-flush-local-all
MPI_Win_sync TD_MPI_WIN_SYNC library=mpi semantic=win-sync
MPI_Win_post TD_MPI_WIN_POST library=mpi semantic=win-post
MPI_Win_start TD_MPI_WIN_START library=mpi semantic=win-start
MPI_Win_complete TD_MPI_WIN_COMPLETE library=mpi semantic=win-complete
MPI_Win_wait TD_MPI_WIN_WAIT library=mpi semantic=win-wait
MPI_Win_test TD_MPI_WIN_TEST library=mpi semantic=win-test
MPI_Comm_dup TD_MPI_COMM_DUP library=mpi semantic=comm-dup
MPI_Comm_dup_with_info TD_MPI_COMM_DUP library=mpi semantic=comm-dup-with-info
MPI_Comm_idup TD_MPI_COMM_DUP library=mpi semantic=comm-idup
MPI_Comm_split TD_MPI_COMM_SPLIT library=mpi semantic=comm-split
MPI_Comm_split_type TD_MPI_COMM_SPLIT library=mpi semantic=comm-split-type
MPI_Comm_create TD_MPI_COMM_CREATE library=mpi semantic=comm-create
MPI_Comm_create_group TD_MPI_COMM_CREATE library=mpi semantic=comm-create-group
MPI_Intercomm_create TD_MPI_COMM_CREATE library=mpi semantic=intercomm-create
MPI_Intercomm_create_from_groups TD_MPI_COMM_CREATE library=mpi semantic=intercomm-create-from-groups
MPI_Intercomm_merge TD_MPI_COMM_CREATE library=mpi semantic=intercomm-merge
MPI_Comm_free TD_MPI_COMM_FREE library=mpi semantic=comm-free
MPI_Comm_disconnect TD_MPI_COMM_FREE library=mpi semantic=comm-disconnect
MPI_Request_free TD_MPI_REQUEST_FREE library=mpi semantic=request-free
MPI_Cancel TD_MPI_CANCEL library=mpi semantic=cancel
MPI_Type_contiguous TD_MPI_TYPE_CONTIGUOUS library=mpi semantic=type-contiguous
MPI_Type_vector TD_MPI_TYPE_VECTOR library=mpi semantic=type-vector
MPI_Type_hvector TD_MPI_TYPE_HVECTOR library=mpi semantic=type-hvector
MPI_Type_indexed TD_MPI_TYPE_INDEXED library=mpi semantic=type-indexed
MPI_Type_hindexed TD_MPI_TYPE_HINDEXED library=mpi semantic=type-hindexed
MPI_Type_struct TD_MPI_TYPE_STRUCT library=mpi semantic=type-struct
MPI_Type_create_dlpack TD_MPI_TYPE_CREATE_DLPACK library=mpi semantic=type-create-dlpack
MPI_Type_commit TD_MPI_TYPE_COMMIT library=mpi semantic=type-commit
MPI_Send_init TD_MPI_PERSISTENT_SEND_INIT library=mpi semantic=persistent-send-init traits=mpi-persistent-init
MPI_Ssend_init TD_MPI_PERSISTENT_SEND_INIT library=mpi semantic=persistent-ssend-init traits=mpi-persistent-init
MPI_Bsend_init TD_MPI_PERSISTENT_SEND_INIT library=mpi semantic=persistent-bsend-init traits=mpi-persistent-init
MPI_Rsend_init TD_MPI_PERSISTENT_SEND_INIT library=mpi semantic=persistent-rsend-init traits=mpi-persistent-init
MPI_Recv_init TD_MPI_PERSISTENT_RECV_INIT library=mpi semantic=persistent-recv-init traits=mpi-persistent-init
MPI_Start TD_MPI_REQUEST_START library=mpi semantic=start traits=mpi-request-start
MPI_Startall TD_MPI_REQUEST_START library=mpi semantic=startall traits=mpi-request-start
