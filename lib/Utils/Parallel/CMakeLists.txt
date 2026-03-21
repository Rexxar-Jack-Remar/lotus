add_library(CanaryParallel STATIC
    ThreadPool.cpp
    Scheduler/Task.cpp
    Scheduler/PipelineScheduler.cpp
    Scheduler/ParallelSchedulerPass.cpp
    lockfree/lockfree_hash_table.cpp
)

target_link_libraries(CanaryParallel PUBLIC LLVMAnalysis CanarySupport)
