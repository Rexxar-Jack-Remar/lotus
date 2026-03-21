add_library(CanaryParallel STATIC
    ThreadPool.cpp
    Scheduler/Task.cpp
    Scheduler/PipelineScheduler.cpp
    Scheduler/ParallelSchedulerPass.cpp
    lockfree/lockfree_hash_table.cpp
)

target_link_libraries(CanaryParallel PUBLIC LLVMAnalysis CanarySupport)

# Some toolchains require the out-of-line libatomic runtime for 16-byte atomics
# used by the lock-free hash table implementation.
find_library(LOTUS_ATOMIC_LIBRARY atomic)
if(LOTUS_ATOMIC_LIBRARY)
    target_link_libraries(CanaryParallel PUBLIC ${LOTUS_ATOMIC_LIBRARY})
endif()
