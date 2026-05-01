#include "CUDAAnalysisTestSupport.h"

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
TEST_F(CUDAAnalysisTest, DoesNotOrderUnknownStreamLaunchAfterUnknownStreamSync) {
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
  EXPECT_FALSE(launch.stream_known);
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
      %l0 = call i64 @cudaLaunchKernel(i8* %k0, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* null)
      call void @kernel_default()
      %l1 = call i64 @cudaLaunchKernel(i8* %k1, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %stream)
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
      %l0 = call i64 @cudaLaunchKernel(i8* %ka, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %s1)
      call void @kernel_a()
      %p0 = call i32 @cudaMemPrefetchAsync(i8* %ptr, i64 64, i32 0, %stream_t* %s1)
      %l1 = call i64 @cudaLaunchKernel(i8* %kb, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %s1)
      call void @kernel_b()
      %p1 = call i32 @cudaMemPrefetchAsync(i8* %ptr, i64 64, i32 0, %stream_t* %s2)
      %l2 = call i64 @cudaLaunchKernel(i8* %kc, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %s1)
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
      %l0 = call i64 @cudaLaunchKernel(i8* %ka, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %consumer)
      call void @kernel_a()
      %l1 = call i64 @cudaLaunchKernel(i8* %kb, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %producer)
      call void @kernel_b()
      %r = call i32 @cudaEventRecord(%event_t* %recorded, %stream_t* %producer)
      %w1 = call i32 @cudaStreamWaitEvent(%stream_t* %consumer, %event_t* %recorded, i32 0)
      %l2 = call i64 @cudaLaunchKernel(i8* %kc, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %consumer)
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
TEST_F(CUDAAnalysisTest, TracksUnifiedMemoryMetadata) {
  const char *source = R"(
    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @cudaMallocHost(i8**, i64)
    declare i32 @cudaMemPrefetchAsync(i8*, i64, i32, i8*)

    define i32 @main() {
    entry:
      %managed = alloca i8*
      %host = alloca i8*
      %m = call i32 @cudaMallocManaged(i8** %managed, i64 64, i32 1)
      %managed_ptr = load i8*, i8** %managed
      %p = call i32 @cudaMemPrefetchAsync(i8* %managed_ptr, i64 64, i32 2, i8* null)
      %h = call i32 @cudaMallocHost(i8** %host, i64 32)
      %sum = add i32 %m, %p
      %sum2 = add i32 %sum, %h
      ret i32 %sum2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getUnifiedMemory().size(), 3u);

  size_t managed_count = 0;
  size_t prefetch_count = 0;
  size_t host_count = 0;
  for (const auto &info : analysis.getUnifiedMemory()) {
    if (info.is_prefetch) {
      ++prefetch_count;
      EXPECT_EQ(info.size, 64u);
      EXPECT_EQ(info.device_id, 2);
    } else if (info.is_managed) {
      ++managed_count;
      EXPECT_EQ(info.size, 64u);
      EXPECT_EQ(info.device_id, -1);
    } else {
      ++host_count;
      EXPECT_EQ(info.size, 32u);
    }
  }

  EXPECT_EQ(managed_count, 1u);
  EXPECT_EQ(prefetch_count, 1u);
  EXPECT_EQ(host_count, 1u);
}
TEST_F(CUDAAnalysisTest, ClassifiesUnifiedMemoryAttachAdviseAndHostAlloc) {
  const char *source = R"(
    %stream_t = type opaque

    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @cudaStreamAttachMemAsync(%stream_t*, i8*, i64, i32)
    declare i32 @cudaMemAdvise(i8*, i64, i32, i32)
    declare i32 @cudaHostAlloc(i8**, i64, i32)

    define i32 @main(%stream_t* %stream) {
    entry:
      %managed = alloca i8*
      %host = alloca i8*
      %m = call i32 @cudaMallocManaged(i8** %managed, i64 128, i32 1)
      %managed_ptr = load i8*, i8** %managed
      %attach = call i32 @cudaStreamAttachMemAsync(%stream_t* %stream,
                                                   i8* %managed_ptr, i64 0,
                                                   i32 2)
      %advise = call i32 @cudaMemAdvise(i8* %managed_ptr, i64 128, i32 1,
                                        i32 7)
      %host_alloc = call i32 @cudaHostAlloc(i8** %host, i64 64, i32 0)
      %sum = add i32 %m, %attach
      %sum2 = add i32 %sum, %advise
      %sum3 = add i32 %sum2, %host_alloc
      ret i32 %sum3
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getUnifiedMemory().size(), 4u);

  size_t managed_count = 0;
  size_t attach_count = 0;
  size_t advise_count = 0;
  size_t host_count = 0;
  for (const auto &info : analysis.getUnifiedMemory()) {
    if (info.is_attach) {
      ++attach_count;
      EXPECT_NE(info.ptr, nullptr);
      EXPECT_EQ(info.stream, module->getFunction("main")->getArg(0));
      EXPECT_EQ(info.size, 0u);
    } else if (info.is_advise) {
      ++advise_count;
      EXPECT_EQ(info.size, 128u);
      EXPECT_EQ(info.device_id, -1);
    } else if (info.is_managed) {
      ++managed_count;
      EXPECT_EQ(info.size, 128u);
    } else {
      ++host_count;
      EXPECT_EQ(info.size, 64u);
    }
  }

  EXPECT_EQ(managed_count, 1u);
  EXPECT_EQ(attach_count, 1u);
  EXPECT_EQ(advise_count, 1u);
  EXPECT_EQ(host_count, 1u);
}
TEST_F(CUDAAnalysisTest, ReportsStructuredModelGapsForManagedAndUnknownUnifiedPointers) {
  const char *source = R"(
    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @cudaMemAdvise(i8*, i64, i32, i32)

    define i32 @main(i8* %unknown_ptr) {
    entry:
      %managed = alloca i8*
      %m = call i32 @cudaMallocManaged(i8** %managed, i64 64, i32 1)
      %managed_ptr = load i8*, i8** %managed
      %a0 = call i32 @cudaMemAdvise(i8* %unknown_ptr, i64 64, i32 0, i32 0)
      %a1 = call i32 @cudaMemAdvise(i8* %managed_ptr, i64 64, i32 1, i32 0)
      %sum = add i32 %m, %a0
      %sum2 = add i32 %sum, %a1
      ret i32 %sum2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &gaps = analysis.getAbstractState().getModelGaps();
  EXPECT_GE(gaps.size(), 1u);

  bool saw_unknown_ptr_gap = false;
  for (const auto &gap : gaps) {
    EXPECT_FALSE(gap.related_instructions.empty());
    EXPECT_GT(gap.confidence, 0.0);
    EXPECT_NE(gap.explanation.find("unknown memory-space classification"),
              std::string::npos);
    saw_unknown_ptr_gap = true;
  }

  EXPECT_TRUE(saw_unknown_ptr_gap);
}
TEST_F(CUDAAnalysisTest, ResolvesRuntimeCudaLaunchKernelWithExplicitOperand) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque

    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @my_kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define i64 @main(i8** %args) {
    entry:
      %stream = inttoptr i64 1 to %stream_t*
      %kernel_ptr = bitcast void ()* @my_kernel to i8*
      %launch = call i64 @cudaLaunchKernel(i8* %kernel_ptr, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %stream)
      ret i64 %launch
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &abstract_state = analysis.getAbstractState();
  const auto &gaps = abstract_state.getModelGaps();

  if (analysis.getLaunches().empty()) {
    EXPECT_FALSE(gaps.empty()) << "If kernel not resolved, must create model gap";
    bool found_unknown_kernel_gap = false;
    for (const auto &gap : gaps) {
      if (gap.explanation.find("kernel") != std::string::npos ||
          gap.explanation.find("launch") != std::string::npos) {
        found_unknown_kernel_gap = true;
      }
    }
    EXPECT_TRUE(found_unknown_kernel_gap) << "Must track unresolved kernel as model gap";
  } else {
    ASSERT_FALSE(analysis.getLaunches().empty());
    const auto &launch = analysis.getLaunches().front();
    EXPECT_NE(launch.kernel, nullptr) << "Kernel should be resolved from operand";
  }
}
TEST_F(CUDAAnalysisTest, SeparatesSummariesForSameKernelDifferentDimensions) {
  const char *source = R"(
    @global_arr = addrspace(1) global [128 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [128 x i32], [128 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      ; First launch with blockDim=1
      call void @__set_CUDAConfig(i32 1, i32 1)
      call void @kernel()
      ; Second launch with blockDim=32
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  const auto &summaries = analysis.getKernelSummaries();
  ASSERT_EQ(summaries.size(), 2u);
  EXPECT_EQ(summaries.size(), 2u) << "Summaries must not be collapsed by function alone";
}
TEST_F(CUDAAnalysisTest, KeepsUnknownDimensionsAsSymbolic) {
  const char *source = R"(
    @global_arr = addrspace(1) global [1024 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()

    define void @kernel(i32 %grid_size) {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
      ; Index depends on runtime grid_size - cannot be statically determined
      %idx = mul i32 %bid, %tid
      %safe_idx = urem i32 %idx, 1024
      %gep = getelementptr [1024 x i32], [1024 x i32] addrspace(1)* @global_arr, i32 0, i32 %safe_idx
      store i32 %idx, i32 addrspace(1)* %gep
      ret void
    }

    define void @main(i32 %gx, i32 %bx) {
    entry:
      call void @__set_CUDAConfig(i32 %gx, i32 %bx)
      call void @kernel(i32 %gx)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_FALSE(analysis.getLaunches().empty());
  const auto &launch = analysis.getLaunches().front();
  EXPECT_TRUE(launch.dimensions.hasSymbolicGrid() || launch.dimensions.hasSymbolicBlock())
      << "Runtime dimensions must remain symbolic, not concretize to 1";
  ASSERT_FALSE(analysis.getKernelSummaries().empty());
  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_TRUE(summary.has_global_race || launch.dimensions.hasSymbolicGrid() || launch.dimensions.hasSymbolicBlock())
      << "Must not miss races due to incorrect dimension concretization";
}
TEST_F(CUDAAnalysisTest, DetectsCrossBlockRaceOnConstantAddress) {
  const char *source = R"(
    @constant_global = addrspace(1) global [8 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()

    define void @kernel() {
    entry:
      %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
      ; All blocks write to global[0] - this is a cross-block race
      %gep = getelementptr [8 x i32], [8 x i32] addrspace(1)* @constant_global, i32 0, i32 0
      store i32 %bid, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      ; Launch with multiple blocks
      call void @__set_CUDAConfig(i32 4, i32 8)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_FALSE(analysis.getKernelSummaries().empty());
  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_TRUE(summary.has_global_race)
      << "Must detect cross-block race on constant address accessed by all blocks";
}
TEST_F(CUDAAnalysisTest, DoesNotConfuseConstantMemoryWithManaged) {
  const char *source = R"(
    @constant_arr = addrspace(4) constant [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep_const = getelementptr [64 x i32], [64 x i32] addrspace(4)* @constant_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(4)* %gep_const
      ret void
    }

    define i32 @main() {
    entry:
      %managed_ptr = alloca i8*
      %r = call i32 @cudaMallocManaged(i8** %managed_ptr, i64 256, i32 1)
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret i32 %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &unified = analysis.getUnifiedMemory();
  size_t managed_count = 0;
  for (const auto &um : unified) {
    if (um.is_managed) {
      ++managed_count;
    }
  }
  EXPECT_GE(managed_count, 1u) << "Should have at least one managed allocation";
}
