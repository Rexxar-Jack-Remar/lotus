#include "CUDAAnalysisTestSupport.h"

TEST_F(CUDAAnalysisTest, DoesNotUseReachabilityOnlyForBarrierOrdering) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare void @llvm.nvvm.barrier0()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %cond = icmp eq i32 %tid, 0
      br i1 %cond, label %left, label %right

    left:
      %left_gep = getelementptr [64 x i32], [64 x i32] addrspace(3)* @shared_arr, i32 0, i32 0
      store i32 %tid, i32 addrspace(3)* %left_gep
      br label %join

    right:
      br label %join

    join:
      call void @llvm.nvvm.barrier0()
      %right_gep = getelementptr [64 x i32], [64 x i32] addrspace(3)* @shared_arr, i32 0, i32 0
      %val = load i32, i32 addrspace(3)* %right_gep
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_TRUE(summary.has_shared_race);
}
TEST_F(CUDAAnalysisTest, SuppressesOrderedInterKernelRaceAfterDeviceSync) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaDeviceSynchronize()
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i32 @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_producer()
      %sync = call i32 @cudaDeviceSynchronize()
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_consumer()
      ret i32 %sync
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_TRUE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::DeviceSynchronize);
  EXPECT_TRUE(analysis.getInterKernelRaces().empty());
}
TEST_F(CUDAAnalysisTest,
       SuppressesInterKernelHazardAfterCrossStreamSynchronize) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque

    declare i32 @cudaStreamSynchronize(%stream_t*)
    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i64 @main(i8* %producer_kernel, i8* %consumer_kernel, i8** %args) {
    entry:
      %producer = inttoptr i64 1 to %stream_t*
      %consumer = inttoptr i64 2 to %stream_t*
      %l0 = call i64 @cudaLaunchKernel(i8* %producer_kernel, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %producer)
      call void @kernel_producer()
      %sync = call i32 @cudaStreamSynchronize(%stream_t* %producer)
      %l1 = call i64 @cudaLaunchKernel(i8* %consumer_kernel, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %consumer)
      call void @kernel_consumer()
      ret i64 %l1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_TRUE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_TRUE(analysis.getLaunches()[1].host_happens_before);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::StreamSynchronize);
  EXPECT_TRUE(analysis.getInterKernelRaces().empty());
}
TEST_F(CUDAAnalysisTest,
       AvoidsClaimingOrderedAfterForUnknownStreamEventSynchronization) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaEventSynchronize(i8*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                        i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                        i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i32 @main(i8* %event) {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_producer()
      %sync = call i32 @cudaEventSynchronize(i8* %event)
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_consumer()
      ret i32 %sync
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_FALSE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::None);

  ASSERT_EQ(analysis.getInterKernelRaces().size(), 1u);
  const auto &race = analysis.getInterKernelRaces().front();
  EXPECT_EQ(race.first_kernel->getName(), "kernel_producer");
  EXPECT_EQ(race.second_kernel->getName(), "kernel_consumer");
  EXPECT_FALSE(race.ordered);
  EXPECT_FALSE(race.stream_known);
}
TEST_F(CUDAAnalysisTest,
       SuppressesInterKernelHazardAfterRecordedEventSynchronization) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque
    %event_t = type opaque

    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaStreamWaitEvent(%stream_t*, %event_t*, i32)
    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i64 @main(%event_t* %event, i8* %producer_kernel, i8* %consumer_kernel,
                     i8** %args) {
    entry:
      %producer = inttoptr i64 1 to %stream_t*
      %consumer = inttoptr i64 2 to %stream_t*
      %l0 = call i64 @cudaLaunchKernel(i8* %producer_kernel, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %producer)
      call void @kernel_producer()
      %record = call i32 @cudaEventRecord(%event_t* %event, %stream_t* %producer)
      %wait = call i32 @cudaStreamWaitEvent(%stream_t* %consumer, %event_t* %event, i32 0)
      %l1 = call i64 @cudaLaunchKernel(i8* %consumer_kernel, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %consumer)
      call void @kernel_consumer()
      ret i64 %l1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_TRUE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_TRUE(analysis.getLaunches()[1].host_happens_before);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::StreamSynchronize);
  EXPECT_TRUE(analysis.getInterKernelRaces().empty());
}
TEST_F(CUDAAnalysisTest,
       AvoidsClaimingOrderedAfterForUnknownStreamMemcpyPrefetchSequence) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaMemcpyAsync(i8*, i8*, i64, i32, i8*)
    declare i32 @cudaMemPrefetchAsync(i8*, i64, i32, i8*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                        i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                        i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i32 @main(i8* %dst, i8* %src) {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_producer()
      %copy = call i32 @cudaMemcpyAsync(i8* %dst, i8* %src, i64 64, i32 1,
                                        i8* null)
      %prefetch = call i32 @cudaMemPrefetchAsync(i8* %dst, i64 64, i32 0,
                                                 i8* null)
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_consumer()
      %sum = add i32 %copy, %prefetch
      ret i32 %sum
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_FALSE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::None);

  ASSERT_EQ(analysis.getMemoryTransfers().size(), 1u);
  EXPECT_TRUE(analysis.getMemoryTransfers().front().is_async);
  EXPECT_EQ(analysis.getMemoryTransfers().front().size, 64u);

  ASSERT_EQ(analysis.getUnifiedMemory().size(), 1u);
  EXPECT_TRUE(analysis.getUnifiedMemory().front().is_prefetch);
  EXPECT_EQ(analysis.getUnifiedMemory().front().size, 64u);

  ASSERT_EQ(analysis.getInterKernelRaces().size(), 1u);
  EXPECT_FALSE(analysis.getInterKernelRaces().front().ordered);
}
TEST_F(CUDAAnalysisTest,
       AvoidsClaimingOrderedAfterForUnknownLaunchAfterStreamWaitEvent) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque
    %event_t = type opaque

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaStreamWaitEvent(%stream_t*, %event_t*, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                        i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                        i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i32 @main(%stream_t* %producer, %event_t* %event) {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_producer()
      %record = call i32 @cudaEventRecord(%event_t* %event,
                                          %stream_t* %producer)
      %wait = call i32 @cudaStreamWaitEvent(%stream_t* null,
                                            %event_t* %event, i32 0)
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_consumer()
      %sum = add i32 %record, %wait
      ret i32 %sum
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_FALSE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::None);

  ASSERT_EQ(analysis.getInterKernelRaces().size(), 1u);
  const auto &race = analysis.getInterKernelRaces().front();
  EXPECT_FALSE(race.ordered);
  EXPECT_FALSE(race.stream_known);
}
TEST_F(CUDAAnalysisTest,
       AvoidsClaimingOrderedAfterForExplicitDefaultStreamLaunches) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare i32 @cudaLaunchKernel(i8*, i32, i32, i32, i32, i64, i8*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                        i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                        i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i32 @main() {
    entry:
      %launch0 = call i32 @cudaLaunchKernel(i8* null, i32 1, i32 1, i32 32,
                                            i32 1, i64 0, i8* null)
      call void @kernel_producer()
      %launch1 = call i32 @cudaLaunchKernel(i8* null, i32 1, i32 1, i32 32,
                                            i32 1, i64 0, i8* null)
      call void @kernel_consumer()
      %sum = add i32 %launch0, %launch1
      ret i32 %sum
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_TRUE(analysis.getLaunches()[0].stream_known);
  EXPECT_TRUE(analysis.getLaunches()[1].stream_known);
  EXPECT_EQ(analysis.getLaunches()[0].stream,
            analysis.getLaunches()[1].stream);
  EXPECT_FALSE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::None);

  ASSERT_EQ(analysis.getInterKernelRaces().size(), 1u);
  const auto &race = analysis.getInterKernelRaces().front();
  EXPECT_FALSE(race.ordered);
  EXPECT_TRUE(race.stream_known);
}
TEST_F(CUDAAnalysisTest, DetectsNonAdjacentUnorderedInterKernelHazard) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_a() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_mid() {
    entry:
      ret void
    }

    define void @kernel_b() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %v = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_a()
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_mid()
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_b()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_FALSE(analysis.getInterKernelRaces().empty());
  bool found = false;
  for (const auto &race : analysis.getInterKernelRaces()) {
    if (race.first_kernel && race.second_kernel &&
        race.first_kernel->getName() == "kernel_a" &&
        race.second_kernel->getName() == "kernel_b") {
      found = true;
      EXPECT_FALSE(race.ordered);
      EXPECT_EQ(race.kind, concurrency::cuda::RaceKind::InterKernelHazard);
    }
  }
  EXPECT_TRUE(found);
}
TEST_F(CUDAAnalysisTest, ReportsMissingFenceForWarpOnlyOrdering) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare void @llvm.nvvm.bar.warp.sync(i32)

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %slot = and i32 %tid, 1
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(3)* @shared_arr, i32 0, i32 %slot
      store i32 %tid, i32 addrspace(3)* %gep
      call void @llvm.nvvm.bar.warp.sync(i32 -1)
      store i32 7, i32 addrspace(3)* %gep
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 64)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.shared_races.empty());
  EXPECT_TRUE(llvm::any_of(summary.shared_races, [](const auto &race) {
    return race.kind == concurrency::cuda::RaceKind::MissingFence;
  }));
}
TEST_F(CUDAAnalysisTest, ReportsAtomicOrderingRiskForMixedAtomicAndNonAtomic) {
  const char *source = R"(
    @global_arr = addrspace(1) global [4 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [4 x i32], [4 x i32] addrspace(1)* @global_arr, i32 0, i32 0
      %old = atomicrmw add i32 addrspace(1)* %gep, i32 1 seq_cst
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.global_races.empty());
  EXPECT_TRUE(llvm::any_of(summary.global_races, [](const auto &race) {
    return race.kind == concurrency::cuda::RaceKind::AtomicOrderingRisk;
  }));
}
TEST_F(CUDAAnalysisTest, SuppressesAtomicOnlyRaceWhenStrongOrderingExists) {
  const char *source = R"(
    @global_arr = addrspace(1) global [4 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)

    define void @kernel() {
    entry:
      %gep = getelementptr [4 x i32], [4 x i32] addrspace(1)* @global_arr, i32 0, i32 0
      %old0 = atomicrmw add i32 addrspace(1)* %gep, i32 1 seq_cst
      %old1 = atomicrmw add i32 addrspace(1)* %gep, i32 1 seq_cst
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_FALSE(summary.has_global_race);
}
TEST_F(CUDAAnalysisTest, DetectsAtomicOnlyMissingFenceWhenOrderingIsWeak) {
  const char *source = R"(
    @global_arr = addrspace(1) global [4 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)

    define void @kernel() {
    entry:
      %gep = getelementptr [4 x i32], [4 x i32] addrspace(1)* @global_arr, i32 0, i32 0
      %old0 = atomicrmw add i32 addrspace(1)* %gep, i32 1 monotonic
      %old1 = atomicrmw add i32 addrspace(1)* %gep, i32 1 monotonic
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.global_races.empty());
  EXPECT_TRUE(llvm::any_of(summary.global_races, [](const auto &race) {
    return race.kind == concurrency::cuda::RaceKind::MissingFence;
  }));
}
TEST_F(CUDAAnalysisTest, ClassifiesAddrSpaceZeroKernelPointerAsGlobal) {
  const char *source = R"(
    define ptx_kernel void @kernel(i32* %param) !nvvm.annotations !0 {
    entry:
      %gep = getelementptr i32, i32* %param, i32 0
      store i32 1, i32* %gep
      ret void
    }

    !0 = !{!1}
    !1 = !{void (i32*)* @kernel, !"kernel", i32 1}
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.accesses.empty());
  const auto &access = summary.accesses.front();
  EXPECT_EQ(access.space, concurrency::cuda::MemorySpace::Global);
  EXPECT_FALSE(access.exact_space);
  EXPECT_EQ(access.base, summary.kernel->getArg(0));
  EXPECT_EQ(access.base_objects.size(), 1u);
  EXPECT_FALSE(access.has_ambiguous_base);
}
TEST_F(CUDAAnalysisTest,
       KeepsAddrSpaceOneKernelPointerPreciseForExistingGlobalCases) {
  const char *source = R"(
    define ptx_kernel void @kernel(i32 addrspace(1)* %param) !nvvm.annotations !0 {
    entry:
      %gep = getelementptr i32, i32 addrspace(1)* %param, i32 0
      store i32 1, i32 addrspace(1)* %gep
      ret void
    }

    !0 = !{!1}
    !1 = !{void (i32 addrspace(1)*)* @kernel, !"kernel", i32 1}
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.accesses.empty());
  const auto &access = summary.accesses.front();
  EXPECT_EQ(access.space, concurrency::cuda::MemorySpace::Global);
  EXPECT_TRUE(access.exact_space);
  EXPECT_EQ(access.base, summary.kernel->getArg(0));
  EXPECT_EQ(access.base_objects.size(), 1u);
  EXPECT_FALSE(access.has_ambiguous_base);
}
TEST_F(CUDAAnalysisTest,
       PreservesMultiBaseAmbiguityForConservativeAddrSpaceZeroKernelPointers) {
  const char *source = R"(
    define ptx_kernel void @kernel(i1 %pick_a, i32* %a, i32* %b) !nvvm.annotations !0 {
    entry:
      %ptr = select i1 %pick_a, i32* %a, i32* %b
      store i32 7, i32* %ptr
      ret void
    }

    !0 = !{!1}
    !1 = !{void (i1, i32*, i32*)* @kernel, !"kernel", i32 1}
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.accesses.empty());
  const auto &access = summary.accesses.front();
  EXPECT_EQ(access.space, concurrency::cuda::MemorySpace::Global);
  EXPECT_FALSE(access.exact_space);
  EXPECT_TRUE(access.has_ambiguous_base);
  EXPECT_EQ(access.base_objects.size(), 2u);
  EXPECT_TRUE(llvm::any_of(access.base_objects, [&](const Value *base) {
    return base == summary.kernel->getArg(1);
  }));
  EXPECT_TRUE(llvm::any_of(access.base_objects, [&](const Value *base) {
    return base == summary.kernel->getArg(2);
  }));
}
TEST_F(CUDAAnalysisTest,
       LeavesUnresolvedAddrSpaceZeroPointersUnknownAndReportsModelGap) {
  const char *source = R"(
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define ptx_kernel void @kernel() !nvvm.annotations !0 {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %addr = zext i32 %tid to i64
      %ptr = inttoptr i64 %addr to i32*
      store i32 %tid, i32* %ptr
      ret void
    }

    !0 = !{!1}
    !1 = !{void ()* @kernel, !"kernel", i32 1}
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.accesses.empty());
  const auto &access = summary.accesses.front();
  EXPECT_EQ(access.space, concurrency::cuda::MemorySpace::Unknown);
  EXPECT_FALSE(access.exact_space);
  EXPECT_FALSE(access.has_ambiguous_base);

  const auto &gaps = analysis.getAbstractState().getModelGaps();
  EXPECT_TRUE(llvm::any_of(gaps, [](const auto &gap) {
    return gap.explanation.find("unknown memory space") != std::string::npos;
  }));
}
TEST_F(CUDAAnalysisTest, ExtractsAffinePatternThroughCastsShiftsAndDivides) {
  const char *source = R"(
    @global_arr = addrspace(1) global [256 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %wide = zext i32 %tid to i64
      %shift = shl i64 %wide, 2
      %half = lshr i64 %shift, 1
      %idx = udiv i64 %half, 2
      %ptr = getelementptr [256 x i32], [256 x i32] addrspace(1)* @global_arr, i64 0, i64 %idx
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.accesses.empty());
  EXPECT_TRUE(summary.accesses.front().address_pattern.valid);
  EXPECT_NE(summary.accesses.front().alias_precision,
            concurrency::cuda::AliasPrecision::NonAffine);
}
TEST_F(CUDAAnalysisTest, CanonicalizesMultidimensionalAffinePattern) {
  const char *source = R"(
    @global_arr = addrspace(1) global [1024 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32, i32, i32, i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.y()

    define void @kernel() {
    entry:
      %tx = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ty = call i32 @llvm.nvvm.read.ptx.sreg.tid.y()
      %row = shl i32 %ty, 5
      %idx = add i32 %row, %tx
      %ptr = getelementptr [1024 x i32], [1024 x i32] addrspace(1)* @global_arr, i32 0, i32 %idx
      store i32 %idx, i32 addrspace(1)* %ptr
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32, i32 1, i32 8, i32 1, i32 1)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.accesses.empty());
  const auto canonical =
      concurrency::cuda::CUDASymbolicModel::normalizeAffineAccessPattern(
          summary.accesses.front().address_pattern, {32, 8, 1}, {1, 1, 1});
  EXPECT_TRUE(canonical.valid);
  EXPECT_EQ(canonical.linear_thread, 4100);
  EXPECT_EQ(canonical.thread_stride_bytes, 4100);
  EXPECT_TRUE(canonical.exact);
}
TEST_F(CUDAAnalysisTest, CanonicalizesThreeDimensionalThreadLinearization) {
  concurrency::cuda::AffineAccessPattern pattern;
  pattern.thread_idx_x = 1;
  pattern.thread_idx_y = 2;
  pattern.thread_idx_z = 3;
  pattern.valid = true;
  pattern.exact = true;

  const auto canonical =
      concurrency::cuda::CUDASymbolicModel::normalizeAffineAccessPattern(
          pattern, {8, 4, 2}, {1, 1, 1});
  EXPECT_TRUE(canonical.valid);
  EXPECT_EQ(canonical.linear_thread, 113);
  EXPECT_EQ(canonical.thread_stride_bytes, 113);
}
TEST_F(CUDAAnalysisTest, DetectsRowVsColumnMajorCoalescing) {
  const char *source = R"(
    @global_arr = addrspace(1) global [1024 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.y()

    ; Row-major: consecutive x within same y row = COALESCED
    define void @kernel_row_major() {
    entry:
      %tid_x = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %tid_y = call i32 @llvm.nvvm.read.ptx.sreg.tid.y()
      %row = mul i32 %tid_y, 32
      %idx = add i32 %row, %tid_x
      %gep = getelementptr [1024 x i32], [1024 x i32] addrspace(1)* @global_arr, i32 0, i32 %idx
      store i32 %idx, i32 addrspace(1)* %gep
      ret void
    }

    ; Column-major: consecutive x = stride by 32 rows = UNCOALESCED
    define void @kernel_col_major() {
    entry:
      %tid_x = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %tid_y = call i32 @llvm.nvvm.read.ptx.sreg.tid.y()
      %idx = add i32 %tid_x, %tid_y
      %gep = getelementptr [1024 x i32], [1024 x i32] addrspace(1)* @global_arr, i32 0, i32 %idx
      store i32 %idx, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 256)
      call void @kernel_row_major()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_FALSE(summary.has_uncoalesced_access);
  for (const auto &issue : summary.coalescing_issues) {
    EXPECT_NE(issue.quality, concurrency::cuda::CoalescingQuality::Uncoalesced);
  }
}
