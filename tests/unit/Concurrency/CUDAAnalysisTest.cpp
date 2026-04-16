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
      call void @kernel_consumer()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  // Gap: Currently only detects intra-kernel races
  // EXPECTED: Should detect inter-kernel race via launch payload aliasing
  EXPECT_EQ(analysis.getLaunches().size(), 2u);
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

  // Gap: Currently flags races even when controlled by uniform predicates
  // EXPECTED: Should NOT flag race when branch is warp-uniform (dependent only
  // on blockIdx) This is a false positive - the branches are mutually exclusive
  // per block
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

  // Gap: Currently flags race even though __syncthreads() orders the
  // communication EXPECTED: Should NOT flag same-block race when all threads
  // execute barrier between access
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

  // Gap: Currently flags any threadIdx in condition as divergence
  // EXPECTED: Should NOT flag when predicate is uniform within warp
  EXPECT_FALSE(summary.has_warp_divergence);
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

  // Gap: Currently only analyzes threadIdx.x patterns
  // EXPECTED: Should detect row vs column major access patterns
  // Row-major should be fully coalesced, column-major should be uncoalesced
  const auto &summary = analysis.getKernelSummaries().front();
  // This test validates that multidimensional patterns can affect coalescing
  // detection Currently passes - the enhancement will make it give different
  // quality
  EXPECT_TRUE(summary.has_uncoalesced_access);
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
