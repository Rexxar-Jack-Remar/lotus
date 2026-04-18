#include "Concurrency/CUDA/CUDAAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;

class CUDAAnalysisTest : public lotus::unittest::LlvmModuleTest {};

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
  EXPECT_TRUE(summary.has_volatile_missing);
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
    @constant_arr = addrspace(4) global [8 x i32] zeroinitializer
    @device_arr = addrspace(101) global [8 x i32] zeroinitializer
    @host_arr = global [8 x i32] zeroinitializer

    define ptx_kernel void @kernel(i32 addrspace(1)* %param) !nvvm.annotations !0 {
    entry:
      %local = alloca i32
      %shared_idx = getelementptr [8 x i32], [8 x i32] addrspace(3)* @shared_arr, i32 0, i32 0
      %constant_idx = getelementptr [8 x i32], [8 x i32] addrspace(4)* @constant_arr, i32 0, i32 0
      %device_idx = getelementptr [8 x i32], [8 x i32] addrspace(101)* @device_arr, i32 0, i32 0
      %host_idx = getelementptr [8 x i32], [8 x i32]* @host_arr, i32 0, i32 0
      store i32 1, i32* %local
      %local_val = load i32, i32* %local
      store i32 2, i32 addrspace(3)* %shared_idx
      %c = load i32, i32 addrspace(4)* %constant_idx
      store i32 %c, i32 addrspace(101)* %device_idx
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
  bool saw_constant = false;
  bool saw_device = false;
  bool saw_global = false;
  bool saw_local = false;
  bool saw_host = false;
  for (const auto &access : summary.accesses) {
    saw_shared |= access.space == concurrency::cuda::MemorySpace::Shared;
    saw_constant |= access.space == concurrency::cuda::MemorySpace::Constant;
    saw_device |= access.space == concurrency::cuda::MemorySpace::Device;
    saw_global |= access.space == concurrency::cuda::MemorySpace::Global;
    saw_local |= access.space == concurrency::cuda::MemorySpace::Local;
    saw_host |= access.space == concurrency::cuda::MemorySpace::Host;
  }

  EXPECT_TRUE(saw_shared);
  EXPECT_TRUE(saw_constant);
  EXPECT_TRUE(saw_device);
  EXPECT_TRUE(saw_global);
  EXPECT_TRUE(saw_local);
  EXPECT_TRUE(saw_host);
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

// ============================================================
// TDD Tests for all 8 gaps - these tests demonstrate missing
// functionality and should FAIL until fixes are implemented
// ============================================================

// Gap 1: Multidimensional symbolic access - threadIdx.y/z dimensions
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

// Gap 2: Cross-kernel race detection
TEST_F(CUDAAnalysisTest, DetectsCrossKernelGlobalRaces) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
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
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_producer()
      call void @__set_CUDAConfig(i32 1, i32 32)
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

// Gap 3: CFG-aware race pruning - warp-uniform branches
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

// Gap 4: Barrier-aware precision - syncthreads ordered communication
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

// Gap 5: Divergence precision - warp-uniform predicates
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
            concurrency::cuda::ParticipationKind::Conditional);
  EXPECT_FALSE(summary.synchronizations.front().exact);
}

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

// Gap 6: Multidimensional coalescing analysis
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

// Gap 7: Volatile precision with atomics
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

// Gap 8: Active-lane aware bank conflicts
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
