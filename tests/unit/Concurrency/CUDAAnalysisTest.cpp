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
  ASSERT_FALSE(summary.coalescing_issues.empty());
  EXPECT_EQ(summary.coalescing_issues.front().quality,
            concurrency::cuda::CoalescingQuality::PartiallyCoalesced);
}

TEST_F(CUDAAnalysisTest, AvoidsFalseRaceAndBankConflictForThreadPrivateIndexing) {
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
