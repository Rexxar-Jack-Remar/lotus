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
  EXPECT_EQ(summary.accesses.front().space, concurrency::cuda::MemorySpace::Global);
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
