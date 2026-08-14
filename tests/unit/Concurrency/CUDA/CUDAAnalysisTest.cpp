#include "CUDAAnalysisTestSupport.h"

TEST_F(CUDAAnalysisTest, SummarizesMemorySpacesAndRisks) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [128 x i32] zeroinitializer
    @shared_stride = addrspace(3) global [128 x i32] zeroinitializer
    @global_arr = addrspace(1) global [128 x i32] zeroinitializer
    @device_arr = addrspace(101) global [32 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32, i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
    declare void @llvm.nvvm.barrier0()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
      %cond = icmp eq i32 %tid, 0
      br i1 %cond, label %then, label %else

    then:
      call void @llvm.nvvm.barrier0()
      br label %merge

    else:
      br label %merge

    merge:
      %mul = mul i32 %tid, 2
      %shared_idx = getelementptr [128 x i32], [128 x i32] addrspace(3)* @shared_arr, i32 0, i32 0
      store i32 %tid, i32 addrspace(3)* %shared_idx
      store i32 %bid, i32 addrspace(3)* %shared_idx
      %shared_stride_idx = getelementptr [128 x i32], [128 x i32] addrspace(3)* @shared_stride, i32 0, i32 %mul
      store i32 %tid, i32 addrspace(3)* %shared_stride_idx
      %global_idx = getelementptr [128 x i32], [128 x i32] addrspace(1)* @global_arr, i32 0, i32 %mul
      store i32 %tid, i32 addrspace(1)* %global_idx
      %global_same = getelementptr [128 x i32], [128 x i32] addrspace(1)* @global_arr, i32 0, i32 0
      store i32 %bid, i32 addrspace(1)* %global_same
      %device_idx = getelementptr [32 x i32], [32 x i32] addrspace(101)* @device_arr, i32 0, i32 %bid
      store i32 %tid, i32 addrspace(101)* %device_idx
      %val = load i32, i32 addrspace(1)* %global_idx
      ret void
    }

    define void @main(i32 %gx, i32 %bx) {
    entry:
      call void @__set_CUDAConfig(i32 %gx, i32 %bx, i32 1, i32 1)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 1u);
  ASSERT_EQ(analysis.getKernelSummaries().size(), 1u);
  const auto &launch = analysis.getLaunches().front();
  EXPECT_TRUE(launch.dimensions.hasSymbolicGrid());
  EXPECT_TRUE(launch.dimensions.hasSymbolicBlock());
  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_EQ(summary.shared_access_count, 3u);
  EXPECT_EQ(summary.global_access_count, 3u);
  EXPECT_EQ(summary.device_access_count, 1u);
  EXPECT_TRUE(summary.has_warp_divergence);
  EXPECT_TRUE(summary.has_barrier_mismatch);
  EXPECT_TRUE(summary.has_shared_race);
  EXPECT_TRUE(summary.has_global_race);
  EXPECT_TRUE(summary.has_bank_conflict);
  EXPECT_TRUE(summary.has_uncoalesced_access);
  EXPECT_FALSE(summary.has_volatile_missing);
  ASSERT_FALSE(summary.bank_conflicts.empty());
  EXPECT_GE(summary.bank_conflicts.front().conflict_degree, 2u);
  EXPECT_GE(summary.bank_conflicts.front().unique_banks, 1u);
  ASSERT_FALSE(summary.coalescing_issues.empty());
  EXPECT_EQ(summary.coalescing_issues.front().quality,
            concurrency::cuda::CoalescingQuality::PartiallyCoalesced);
  EXPECT_GT(summary.coalescing_issues.front().covered_bytes, 0u);
}
TEST_F(CUDAAnalysisTest, PopulatesAccessFactsInAbstractState) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %idx = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %idx
      store i32 %val, i32 addrspace(1)* %idx
      ret void
    }

    define i32 @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 1)
      call void @kernel()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 1u);

  const auto &abstract_state = analysis.getAbstractState();
  const auto &access_facts = abstract_state.getAccessFacts();
  EXPECT_FALSE(access_facts.empty());

  size_t write_count = 0;
  size_t read_count = 0;
  for (const auto &fact : access_facts) {
    if (fact.is_write) {
      ++write_count;
    } else {
      ++read_count;
    }
  }
  EXPECT_EQ(read_count, 1u);
  EXPECT_EQ(write_count, 1u);

  EXPECT_TRUE(abstract_state.getModelGaps().empty());
}
TEST_F(CUDAAnalysisTest,
       AvoidsFalseRaceAndBankConflictForThreadPrivateIndexing) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [64 x i32] zeroinitializer
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %shared_idx = getelementptr [64 x i32], [64 x i32] addrspace(3)* @shared_arr, i32 0, i32 %tid
      store volatile i32 %tid, i32 addrspace(3)* %shared_idx
      %global_idx = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store volatile i32 %tid, i32 addrspace(1)* %global_idx
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
  ASSERT_EQ(analysis.getKernelSummaries().size(), 1u);
  const auto &summary = analysis.getKernelSummaries().front();

  EXPECT_FALSE(summary.has_shared_race);
  EXPECT_FALSE(summary.has_bank_conflict);
  EXPECT_FALSE(summary.has_uncoalesced_access);
  EXPECT_FALSE(summary.has_volatile_missing);
}
TEST_F(CUDAAnalysisTest, DetectsCrossBlockGlobalRacesWhenBlockIdxCollides) {
  const char *source = R"(
    @global_arr = addrspace(1) global [16 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()

    define void @kernel() {
    entry:
      %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
      %slot = and i32 %bid, 1
      %global_idx = getelementptr [16 x i32], [16 x i32] addrspace(1)* @global_arr, i32 0, i32 %slot
      store i32 %bid, i32 addrspace(1)* %global_idx
      store i32 7, i32 addrspace(1)* %global_idx
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 4, i32 32)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getKernelSummaries().size(), 1u);
  const auto &summary = analysis.getKernelSummaries().front();

  EXPECT_TRUE(summary.has_global_race);
  ASSERT_FALSE(summary.global_races.empty());
  EXPECT_TRUE(summary.global_races.front().cross_block);
  EXPECT_FALSE(summary.global_races.front().symbolic);
}
TEST_F(CUDAAnalysisTest, ClassifiesNVVMMemorySpacesPrecisely) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [8 x i32] zeroinitializer
    @cluster_arr = addrspace(7) global [8 x i32] zeroinitializer
    @constant_arr = addrspace(4) global [8 x i32] zeroinitializer
    @legacy_arr = addrspace(101) global [8 x i32] zeroinitializer
    @host_arr = global [8 x i32] zeroinitializer

    define ptx_kernel void @kernel(i32 addrspace(1)* %param) !nvvm.annotations !0 {
    entry:
      %local = alloca i32
      %shared_idx = getelementptr [8 x i32], [8 x i32] addrspace(3)* @shared_arr, i32 0, i32 0
      %cluster_idx = getelementptr [8 x i32], [8 x i32] addrspace(7)* @cluster_arr, i32 0, i32 0
      %constant_idx = getelementptr [8 x i32], [8 x i32] addrspace(4)* @constant_arr, i32 0, i32 0
      %legacy_idx = getelementptr [8 x i32], [8 x i32] addrspace(101)* @legacy_arr, i32 0, i32 0
      %host_idx = getelementptr [8 x i32], [8 x i32]* @host_arr, i32 0, i32 0
      store i32 1, i32* %local
      %local_val = load i32, i32* %local
      store i32 2, i32 addrspace(3)* %shared_idx
      store i32 3, i32 addrspace(7)* %cluster_idx
      %c = load i32, i32 addrspace(4)* %constant_idx
      store i32 %c, i32 addrspace(101)* %legacy_idx
      store i32 %local_val, i32 addrspace(1)* %param
      store i32 5, i32* %host_idx
      ret void
    }

    !0 = !{!1}
    !1 = !{void (i32 addrspace(1)*)* @kernel, !"kernel", i32 1}
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getKernelSummaries().size(), 1u);
  const auto &summary = analysis.getKernelSummaries().front();

  bool saw_shared = false;
  bool saw_cluster_shared = false;
  bool saw_constant = false;
  bool saw_device = false;
  bool saw_global = false;
  bool saw_local = false;
  bool saw_host = false;
  bool saw_unknown = false;
  for (const auto &access : summary.accesses) {
    saw_shared |= access.space == concurrency::cuda::MemorySpace::Shared;
    saw_cluster_shared |=
        access.space == concurrency::cuda::MemorySpace::ClusterShared;
    saw_constant |= access.space == concurrency::cuda::MemorySpace::Constant;
    saw_device |= access.space == concurrency::cuda::MemorySpace::Device;
    saw_global |= access.space == concurrency::cuda::MemorySpace::Global;
    saw_local |= access.space == concurrency::cuda::MemorySpace::Local;
    saw_host |= access.space == concurrency::cuda::MemorySpace::Host;
    saw_unknown |= access.space == concurrency::cuda::MemorySpace::Unknown;
  }

  EXPECT_TRUE(saw_shared);
  EXPECT_TRUE(saw_cluster_shared);
  EXPECT_TRUE(saw_constant);
  EXPECT_FALSE(saw_device);
  EXPECT_TRUE(saw_global);
  EXPECT_TRUE(saw_local);
  EXPECT_TRUE(saw_host);
  EXPECT_TRUE(saw_unknown);
}
TEST_F(CUDAAnalysisTest, BuildsStreamAndEventAutomataForAsyncRuntimeOps) {
  const char *source = R"(
    %stream_t = type opaque
    %event_t = type opaque

    @global_arr = addrspace(1) global [32 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaStreamCreate(%stream_t**)
    declare i32 @cudaStreamSynchronize(%stream_t*)
    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaEventSynchronize(%event_t*)
    declare i32 @cudaMemcpyAsync(i8*, i8*, i64, i32, %stream_t*)
    declare i32 @cudaMemPrefetchAsync(i8*, i64, i32, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr [32 x i32], [32 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }

    define i32 @main(%stream_t** %stream_slot, %event_t* %event, i8* %dst, i8* %src) {
    entry:
      %stream = load %stream_t*, %stream_t** %stream_slot
      %c0 = call i32 @cudaStreamCreate(%stream_t** %stream_slot)
      %c1 = call i32 @cudaMemcpyAsync(i8* %dst, i8* %src, i64 64, i32 1, %stream_t* %stream)
      %c2 = call i32 @cudaEventRecord(%event_t* %event, %stream_t* %stream)
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      %c3 = call i32 @cudaMemPrefetchAsync(i8* %dst, i64 64, i32 0, %stream_t* %stream)
      %c4 = call i32 @cudaStreamSynchronize(%stream_t* %stream)
      %c5 = call i32 @cudaEventSynchronize(%event_t* %event)
      ret i32 %c5
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 1u);
  EXPECT_FALSE(analysis.getStreamAutomata().empty());
  EXPECT_FALSE(analysis.getEventAutomata().empty());
  ASSERT_GE(analysis.getMemoryTransfers().size(), 1u);
  EXPECT_FALSE(analysis.getUnifiedMemory().empty());
  EXPECT_TRUE(analysis.getUnifiedMemory().front().is_prefetch);
  EXPECT_NE(analysis.getUnifiedMemory().front().stream, nullptr);
}
TEST_F(CUDAAnalysisTest, TracksStreamWaitEventOrderingAcrossAutomata) {
  const char *source = R"(
    %stream_t = type opaque
    %event_t = type opaque

    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaStreamWaitEvent(%stream_t*, %event_t*, i32)
    declare i32 @cudaStreamSynchronize(%stream_t*)

    define i32 @main(%stream_t* %producer, %stream_t* %consumer,
                     %event_t* %event) {
    entry:
      %record = call i32 @cudaEventRecord(%event_t* %event,
                                          %stream_t* %producer)
      %wait = call i32 @cudaStreamWaitEvent(%stream_t* %consumer,
                                            %event_t* %event, i32 0)
      %sync = call i32 @cudaStreamSynchronize(%stream_t* %consumer)
      %sum = add i32 %record, %wait
      %sum2 = add i32 %sum, %sync
      ret i32 %sum2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getStreamAutomata().size(), 2u);
  ASSERT_EQ(analysis.getEventAutomata().size(), 1u);

  const auto &streams = analysis.getStreamAutomata();
  auto consumer_it = llvm::find_if(streams, [&](const auto &automaton) {
    return automaton.stream == module->getFunction("main")->getArg(1);
  });
  ASSERT_NE(consumer_it, streams.end());
  EXPECT_TRUE(consumer_it->is_ordered);
  ASSERT_EQ(consumer_it->transitions.size(), 2u);
  EXPECT_TRUE(consumer_it->transitions.front().is_ordering_boundary);
  EXPECT_EQ(consumer_it->current_state,
            concurrency::cuda::StreamState::Synchronized);
  EXPECT_TRUE(consumer_it->pending_operations.empty());

  const auto &event = analysis.getEventAutomata().front();
  EXPECT_TRUE(event.has_record);
  EXPECT_TRUE(event.has_wait);
  EXPECT_EQ(event.recorded_stream, module->getFunction("main")->getArg(0));
  ASSERT_EQ(event.transitions.size(), 2u);
  EXPECT_EQ(event.transitions.front().to_state,
            concurrency::cuda::EventState::Recorded);
  EXPECT_EQ(event.transitions.back().to_state,
            concurrency::cuda::EventState::Waited);
  ASSERT_EQ(event.pending_waits.size(), 1u);
}
TEST_F(CUDAAnalysisTest, SummarizesExtendedCUDAOperationsInterprocedurally) {
  const char *source = R"(
    %stream_t = type opaque
    %event_t = type opaque

    declare i32 @cudaStreamSynchronize(%stream_t*)
    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaMemcpyAsync(i8*, i8*, i64, i32, %stream_t*)

    define void @helper(%stream_t* %stream, %event_t* %event, i8* %dst, i8* %src) {
    entry:
      %m = call i32 @cudaMemcpyAsync(i8* %dst, i8* %src, i64 16, i32 1, %stream_t* %stream)
      %e = call i32 @cudaEventRecord(%event_t* %event, %stream_t* %stream)
      ret void
    }

    define void @wrapper(%stream_t* %stream, %event_t* %event, i8* %dst, i8* %src) {
    entry:
      call void @helper(%stream_t* %stream, %event_t* %event, i8* %dst, i8* %src)
      %s = call i32 @cudaStreamSynchronize(%stream_t* %stream)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  const Function *wrapper = module->getFunction("wrapper");
  ASSERT_NE(wrapper, nullptr);
  auto it = summaries.find(wrapper);
  ASSERT_NE(it, summaries.end());
  EXPECT_EQ(it->second.memory_transfers.size(), 1u);
  EXPECT_EQ(it->second.stream_ops.size(), 1u);
  EXPECT_EQ(it->second.event_ops.size(), 1u);
}
TEST_F(CUDAAnalysisTest, TracksTextureAndSurfaceAccesses) {
  const char *source = R"(
    declare i32 @tex1Dfetch(i64, i32)
    declare void @surf1Dwrite(i32, i64, i32)

    define ptx_kernel void @kernel(i64 %tex, i64 %surf) !nvvm.annotations !0 {
    entry:
      %t = call i32 @tex1Dfetch(i64 %tex, i32 0)
      call void @surf1Dwrite(i32 %t, i64 %surf, i32 0)
      ret void
    }

    !0 = !{!1}
    !1 = !{void (i64, i64)* @kernel, !"kernel", i32 1}
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getKernelSummaries().size(), 1u);
  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_TRUE(summary.has_texture_access);
  EXPECT_TRUE(summary.has_surface_access);
  EXPECT_EQ(summary.texture_accesses.size(), 1u);
  EXPECT_EQ(summary.surface_accesses.size(), 1u);
}
TEST_F(CUDAAnalysisTest, TracksAmbiguousBaseSetsThroughSelectPointers) {
  const char *source = R"(
    @global_a = addrspace(1) global [8 x i32] zeroinitializer
    @global_b = addrspace(1) global [8 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel(i1 %pick_a) {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %a = getelementptr [8 x i32], [8 x i32] addrspace(1)* @global_a, i32 0, i32 %tid
      %b = getelementptr [8 x i32], [8 x i32] addrspace(1)* @global_b, i32 0, i32 %tid
      %ptr = select i1 %pick_a, i32 addrspace(1)* %a, i32 addrspace(1)* %b
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }

    define void @main(i1 %pick_a) {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel(i1 %pick_a)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getKernelSummaries().size(), 1u);

  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.accesses.empty());
  const auto &access = summary.accesses.front();
  EXPECT_TRUE(access.has_ambiguous_base);
  EXPECT_GE(access.base_objects.size(), 2u);
}
TEST_F(CUDAAnalysisTest,
       TracksSymbolicParametricRaceAndImpreciseMemoryClassification) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [128 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel(i32* %mystery) {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %slot = and i32 %tid, 1
      %shared_idx = getelementptr [128 x i32], [128 x i32] addrspace(3)* @shared_arr, i32 0, i32 %slot
      store i32 %tid, i32 addrspace(3)* %shared_idx
      store i32 7, i32 addrspace(3)* %shared_idx
      %ptr = getelementptr i32, i32* %mystery, i32 %slot
      store i32 %tid, i32* %ptr
      store i32 9, i32* %ptr
      ret void
    }

    define void @main(i32 %sym_block, i32* %mystery) {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 %sym_block)
      call void @kernel(i32* %mystery)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getKernelSummaries().size(), 1u);
  const auto &summary = analysis.getKernelSummaries().front();

  EXPECT_TRUE(summary.dimensions.hasSymbolicBlock());
  EXPECT_TRUE(summary.has_shared_race);
  ASSERT_FALSE(summary.shared_races.empty());
  EXPECT_TRUE(summary.shared_races.front().symbolic);

  bool saw_imprecise = false;
  for (const auto &access : summary.accesses) {
    if (access.pointer &&
        access.pointer->getType()->getPointerAddressSpace() == 0 &&
        !access.exact_space) {
      saw_imprecise = true;
    }
  }
  EXPECT_TRUE(saw_imprecise);
}
TEST_F(CUDAAnalysisTest, ExtractsMultidimensionalLaunchParameters) {
  const char *source = R"(
    @global_arr = addrspace(1) global [1024 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32, i32, i32, i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.y()
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.z()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.y()

    define void @kernel_2d() {
    entry:
      %tid_x = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %tid_y = call i32 @llvm.nvvm.read.ptx.sreg.tid.y()
      ; Access using 2D thread index for row-major storage
      %row = mul i32 %tid_y, 32
      %idx = add i32 %row, %tid_x
      %gep = getelementptr [1024 x i32], [1024 x i32] addrspace(1)* @global_arr, i32 0, i32 %idx
      store i32 %idx, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      ; grid.x, block.x, grid.y, block.y, grid.z, block.z
      call void @__set_CUDAConfig(i32 4, i32 8, i32 2, i32 16, i32 1, i32 1)
      call void @kernel_2d()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  auto &dims = analysis.getLaunches().front().dimensions;
  // Currently: only extracts grid[0] and block[0]
  // EXPECTED: Extracts all 6 dimensions
  EXPECT_EQ(dims.grid[0].kind, concurrency::cuda::SymbolicValueKind::Constant);
  EXPECT_EQ(dims.grid[0].constant, 4u);
  EXPECT_EQ(dims.grid[1].kind, concurrency::cuda::SymbolicValueKind::Constant);
  EXPECT_EQ(dims.grid[1].constant, 2u);
  EXPECT_EQ(dims.grid[2].kind, concurrency::cuda::SymbolicValueKind::Constant);
  EXPECT_EQ(dims.grid[2].constant, 1u);
  EXPECT_EQ(dims.block[0].kind, concurrency::cuda::SymbolicValueKind::Constant);
  EXPECT_EQ(dims.block[0].constant, 8u);
  EXPECT_EQ(dims.block[1].kind, concurrency::cuda::SymbolicValueKind::Constant);
  EXPECT_EQ(dims.block[1].constant, 16u);
  EXPECT_EQ(dims.block[2].kind, concurrency::cuda::SymbolicValueKind::Constant);
  EXPECT_EQ(dims.block[2].constant, 1u);
}
TEST_F(CUDAAnalysisTest, DetectsCrossKernelGlobalRaces) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque

    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64,
                                  i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()

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

    define void @main(i32* %ptr) {
    entry:
      %s1 = inttoptr i64 10 to %stream_t*
      %s2 = inttoptr i64 20 to %stream_t*
      %l0 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @kernel_producer to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s1)
      call void @kernel_producer()
      %l1 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @kernel_consumer to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s2)
      call void @kernel_consumer()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getLaunches().size(), 2u);
  ASSERT_EQ(analysis.getInterKernelRaces().size(), 1u);
  const auto &race = analysis.getInterKernelRaces().front();
  EXPECT_EQ(race.first_kernel->getName(), "kernel_producer");
  EXPECT_EQ(race.second_kernel->getName(), "kernel_consumer");
  EXPECT_FALSE(race.ordered);
}
TEST_F(CUDAAnalysisTest, AvoidsFalseRaceForUniformBranchControlledAccesses) {
  const char *source = R"(
    @global_arr = addrspace(1) global [32 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()

    define void @kernel_uniform() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
      ; Uniform across all threads in warp: blockIdx.x
      %cond = icmp eq i32 %bid, 0
      br i1 %cond, label %then, label %else

    then:
      %gep_then = getelementptr [32 x i32], [32 x i32] addrspace(1)* @global_arr, i32 0, i32 0
      store i32 %tid, i32 addrspace(1)* %gep_then
      br label %merge

    else:
      %gep_else = getelementptr [32 x i32], [32 x i32] addrspace(1)* @global_arr, i32 0, i32 1
      store i32 %tid, i32 addrspace(1)* %gep_else
      br label %merge

    merge:
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_uniform()
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
TEST_F(CUDAAnalysisTest, AvoidsFalseRaceForBarrierProtectedSharedAccess) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [32 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare void @llvm.nvvm.barrier0()

    define void @kernel_sync() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      ; Phase 1: write to shared with thread index
      %gep1 = getelementptr [32 x i32], [32 x i32] addrspace(3)* @shared_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(3)* %gep1
      ; Synchronize - all threads in CTA must reach here
      call void @llvm.nvvm.barrier0()
      ; Phase 2: read what others wrote
      %gep2 = getelementptr [32 x i32], [32 x i32] addrspace(3)* @shared_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(3)* %gep2
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_sync()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summary = analysis.getKernelSummaries().front();

  EXPECT_FALSE(summary.has_shared_race);
}
TEST_F(CUDAAnalysisTest, AvoidsFalseDivergenceForUniformPredicates) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ntid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ntid = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
      ; Uniform within warp: depends on block-level quantity
      %cond = icmp ugt i32 %ntid, 16
      br i1 %cond, label %then, label %else

    then:
      br label %merge

    else:
      br label %merge

    merge:
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

  EXPECT_FALSE(summary.has_warp_divergence);
}
TEST_F(CUDAAnalysisTest, TreatsCastBuiltinConditionsAsUniform) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.ntid.x()

    define void @kernel() {
    entry:
      %ntid = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
      %wide = zext i32 %ntid to i64
      %cond = icmp ugt i64 %wide, 16
      br i1 %cond, label %then, label %else

    then:
      br label %merge

    else:
      br label %merge

    merge:
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
  EXPECT_FALSE(summary.has_warp_divergence);
  ASSERT_FALSE(summary.warp_uniform_regions.empty());
  EXPECT_TRUE(summary.warp_uniform_regions.front().uniform_within_warp);
}
TEST_F(CUDAAnalysisTest, MarksBarrierMustReachOnlyWhenAllThreadsReach) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare void @llvm.nvvm.barrier0()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %cond = icmp eq i32 %tid, 0
      br i1 %cond, label %then, label %else

    then:
      call void @llvm.nvvm.barrier0()
      br label %merge

    else:
      br label %merge

    merge:
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
  ASSERT_FALSE(summary.synchronizations.empty());
  EXPECT_EQ(summary.synchronizations.front().primitive,
            concurrency::cuda::SynchronizationPrimitive::BlockBarrier);
  EXPECT_EQ(summary.synchronizations.front().participation,
            concurrency::cuda::ParticipationKind::Conditional);
  EXPECT_FALSE(summary.synchronizations.front().exact);
}
TEST_F(CUDAAnalysisTest, DoesNotTreatPartialWarpMaskAsExactWarpOrdering) {
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
      call void @llvm.nvvm.bar.warp.sync(i32 255)
      %val = load i32, i32 addrspace(3)* %gep
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
  ASSERT_FALSE(summary.synchronizations.empty());
  EXPECT_EQ(summary.synchronizations.front().primitive,
            concurrency::cuda::SynchronizationPrimitive::WarpBarrier);
  EXPECT_EQ(summary.synchronizations.front().participation,
            concurrency::cuda::ParticipationKind::Partial);
  EXPECT_FALSE(summary.synchronizations.front().exact);
}
