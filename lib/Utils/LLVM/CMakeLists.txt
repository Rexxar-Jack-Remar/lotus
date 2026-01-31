add_library(CanaryLLVMUtils STATIC
        Debug.cpp
        InstructionUtils.cpp
        RecursiveTimer.cpp
        Statistics.cpp
        StringUtils.cpp
        ThreadPool.cpp
        IO/FileUtils.cpp
        IO/ReadIR.cpp
        IO/WriteIR.cpp
        Scheduler/Task.cpp
        Scheduler/PipelineScheduler.cpp
        Scheduler/ParallelSchedulerPass.cpp
        Demangle.cpp
)

# Link against LLVMDemangle since Demangle.cpp uses llvm::demangle()
# PipelineScheduler uses ProgressBar from CanarySupport
target_link_libraries(CanaryLLVMUtils PUBLIC LLVMDemangle LLVMAnalysis CanarySupport)
