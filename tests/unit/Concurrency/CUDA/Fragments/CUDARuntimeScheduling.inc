TEST_F(CUDAAnalysisTest, AvoidsFalseVolatileWarningForAtomicCommunication) {
  const char *source = R"(
    @global_arr = addrspace(1) global [32 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_atomic() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [32 x i32], [32 x i32] addrspace(1)* @global_arr, i32 0, i32 0
      ; Atomic operation provides synchronization
      %old = atomicrmw add i32 addrspace(1)* %gep, i32 %tid seq_cst
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_atomic()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();

  // Gap: Currently flags any threadIdx access without volatile
  // EXPECTED: Should NOT warn when atomic operations are used for
  // synchronization
  EXPECT_FALSE(summary.has_volatile_missing);
}
TEST_F(CUDAAnalysisTest, ReportsAccurateBankConflictWithActiveLanes) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [32 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
    declare void @llvm.nvvm.barrier0()

    define void @kernel_predicated() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ntid = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
      ; Only half the warp is active
      %cond = icmp ult i32 %tid, 16
      br i1 %cond, label %active, label %inactive

    active:
      %gep = getelementptr [32 x i32], [32 x i32] addrspace(3)* @shared_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(3)* %gep
      br label %merge

    inactive:
      br label %merge

    merge:
      call void @llvm.nvvm.barrier0()
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_predicated()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();

  // Gap: Currently assumes all 32 lanes active
  // EXPECTED: Bank conflict degree should reflect actual active lanes (16, not
  // 32) This is a precision issue - currently may over-report conflict degree
  if (summary.has_bank_conflict) {
    EXPECT_LE(summary.bank_conflicts.front().conflict_degree, 16u);
  }
}
TEST_F(CUDAAnalysisTest, UnknownStreamSyncDoesNotOrderFollowingDefaultLaunch) {
  const char *source = R"(
    %stream_t = type opaque

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaStreamSynchronize(%stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ret void
    }

    define i32 @main(%stream_t** %slot) {
    entry:
      %stream = load %stream_t*, %stream_t** %slot
      %sync = call i32 @cudaStreamSynchronize(%stream_t* %stream)
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret i32 %sync
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 1u);
  const auto &launch = analysis.getLaunches().front();
  EXPECT_TRUE(launch.stream_known);
  EXPECT_EQ(launch.stream_kind,
            concurrency::cuda::HostStreamKind::LegacyDefault);
  EXPECT_FALSE(launch.ordered_after_previous);
  EXPECT_FALSE(launch.host_happens_before);
  EXPECT_EQ(launch.ordering_source, concurrency::cuda::LaunchOrderingSource::None);
}
TEST_F(CUDAAnalysisTest, OrdersLegacyDefaultStreamBeforeExplicitStreamLaunch) {
  const char *source = R"(
    %stream_t = type opaque
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_default() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_explicit() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i64 @main(i8* %k0, i8* %k1, i8** %args) {
    entry:
      %stream = inttoptr i64 1 to %stream_t*
      %l0 = call i64 @cudaLaunchKernel(i8* bitcast (void ()* @kernel_default to i8*), i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* null)
      call void @kernel_default()
      %l1 = call i64 @cudaLaunchKernel(i8* bitcast (void ()* @kernel_explicit to i8*), i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %stream)
      call void @kernel_explicit()
      ret i64 %l1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_TRUE(analysis.getLaunches()[0].stream_known);
  EXPECT_EQ(analysis.getLaunches()[0].stream_kind,
            concurrency::cuda::HostStreamKind::LegacyDefault);
  EXPECT_TRUE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_TRUE(analysis.getLaunches()[1].host_happens_before);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::ProgramOrder);
}
TEST_F(CUDAAnalysisTest, OrdersLaunchAfterSameStreamAsyncTransfer) {
  const char *source = R"(
    %stream_t = type opaque

    declare i32 @cudaMemcpyAsync(i8*, i8*, i64, i32, %stream_t*)
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ret void
    }

    define i32 @main(i8* %dst, i8* %src) {
    entry:
      %stream = inttoptr i64 1 to %stream_t*
      %copy = call i32 @cudaMemcpyAsync(i8* %dst, i8* %src, i64 64, i32 1, %stream_t* %stream)
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret i32 %copy
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 1u);
  const auto &launch = analysis.getLaunches().front();
  EXPECT_TRUE(launch.stream_known);
  EXPECT_TRUE(launch.ordered_after_previous);
  EXPECT_TRUE(launch.host_happens_before);
  EXPECT_EQ(launch.ordering_source, concurrency::cuda::LaunchOrderingSource::ProgramOrder);
  EXPECT_EQ(launch.predecessor,
            concurrency::cuda::SynchronizationPrimitive::StreamProgramOrder);
}
TEST_F(CUDAAnalysisTest, PrefetchOrderingRequiresMatchingConcreteStream) {
  const char *source = R"(
    %stream_t = type opaque

    declare i32 @cudaMemPrefetchAsync(i8*, i64, i32, %stream_t*)
    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_a() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ret void
    }

    define void @kernel_b() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ret void
    }

    define void @kernel_c() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ret void
    }

    define i64 @main(i8* %ptr, i8* %ka, i8* %kb, i8* %kc, i8** %args) {
    entry:
      %s1 = inttoptr i64 1 to %stream_t*
      %s2 = inttoptr i64 2 to %stream_t*
      %l0 = call i64 @cudaLaunchKernel(i8* bitcast (void ()* @kernel_a to i8*), i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %s1)
      call void @kernel_a()
      %p0 = call i32 @cudaMemPrefetchAsync(i8* %ptr, i64 64, i32 0, %stream_t* %s1)
      %l1 = call i64 @cudaLaunchKernel(i8* bitcast (void ()* @kernel_b to i8*), i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %s1)
      call void @kernel_b()
      %p1 = call i32 @cudaMemPrefetchAsync(i8* %ptr, i64 64, i32 0, %stream_t* %s2)
      %l2 = call i64 @cudaLaunchKernel(i8* bitcast (void ()* @kernel_c to i8*), i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %s1)
      call void @kernel_c()
      ret i64 %l2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 3u);
  EXPECT_TRUE(analysis.getLaunches()[1].ordered_after_previous);
  EXPECT_TRUE(analysis.getLaunches()[1].host_happens_before);
  EXPECT_EQ(analysis.getLaunches()[1].ordering_source,
            concurrency::cuda::LaunchOrderingSource::ProgramOrder);
  EXPECT_TRUE(analysis.getLaunches()[2].ordered_after_previous);
  EXPECT_TRUE(analysis.getLaunches()[2].host_happens_before);
  EXPECT_EQ(analysis.getLaunches()[2].ordering_source,
            concurrency::cuda::LaunchOrderingSource::ProgramOrder);
}
TEST_F(CUDAAnalysisTest, EventWaitNeedsMatchingRecordedEventToOrderLaunch) {
  const char *source = R"(
    %stream_t = type opaque
    %event_t = type opaque

    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaStreamWaitEvent(%stream_t*, %event_t*, i32)
    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_a() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ret void
    }

    define void @kernel_b() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ret void
    }

    define void @kernel_c() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ret void
    }

    define i64 @main(%event_t* %recorded, %event_t* %missing,
                     i8* %ka, i8* %kb, i8* %kc, i8** %args) {
    entry:
      %producer = inttoptr i64 1 to %stream_t*
      %consumer = inttoptr i64 2 to %stream_t*
      %w0 = call i32 @cudaStreamWaitEvent(%stream_t* %consumer, %event_t* %missing, i32 0)
      %l0 = call i64 @cudaLaunchKernel(i8* bitcast (void ()* @kernel_a to i8*), i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %consumer)
      call void @kernel_a()
      %l1 = call i64 @cudaLaunchKernel(i8* bitcast (void ()* @kernel_b to i8*), i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %producer)
      call void @kernel_b()
      %r = call i32 @cudaEventRecord(%event_t* %recorded, %stream_t* %producer)
      %w1 = call i32 @cudaStreamWaitEvent(%stream_t* %consumer, %event_t* %recorded, i32 0)
      %l2 = call i64 @cudaLaunchKernel(i8* bitcast (void ()* @kernel_c to i8*), i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %consumer)
      call void @kernel_c()
      ret i64 %l2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 3u);
  EXPECT_FALSE(analysis.getLaunches()[0].ordered_after_previous);
  EXPECT_EQ(analysis.getLaunches()[0].ordering_source,
            concurrency::cuda::LaunchOrderingSource::None);
  EXPECT_TRUE(analysis.getLaunches()[2].ordered_after_previous);
  EXPECT_TRUE(analysis.getLaunches()[2].host_happens_before);
  EXPECT_EQ(analysis.getLaunches()[2].predecessor,
            concurrency::cuda::SynchronizationPrimitive::StreamProgramOrder);
}
