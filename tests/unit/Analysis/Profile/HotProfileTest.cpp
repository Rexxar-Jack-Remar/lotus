#include "Analysis/Profile/Hot.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>

#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/BranchProbabilityInfo.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>

namespace {

using lotus::analysis::profile::Hot;
using lotus::unittest::findBlock;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

struct FunctionProfileAnalyses {
  explicit FunctionProfileAnalyses(llvm::Function &function)
      : dom_tree(function), loop_info(dom_tree), branch_prob(function, loop_info),
        block_freq(function, branch_prob, loop_info) {}

  llvm::DominatorTree dom_tree;
  llvm::LoopInfo loop_info;
  llvm::BranchProbabilityInfo branch_prob;
  llvm::BlockFrequencyInfo block_freq;
};

class ProfileAnalysisCache {
public:
  llvm::BlockFrequencyInfo &getBFI(llvm::Function &function) {
    return get(function).block_freq;
  }

  llvm::BranchProbabilityInfo &getBPI(llvm::Function &function) {
    return get(function).branch_prob;
  }

  llvm::LoopInfo &getLoopInfo(llvm::Function &function) {
    return get(function).loop_info;
  }

private:
  FunctionProfileAnalyses &get(llvm::Function &function) {
    auto [it, inserted] = analyses.emplace(&function, nullptr);
    if (inserted) {
      it->second = std::make_unique<FunctionProfileAnalyses>(function);
    }
    return *it->second;
  }

  std::map<llvm::Function *, std::unique_ptr<FunctionProfileAnalyses>> analyses;
};

using lotus::unittest::findBlock;
using lotus::unittest::findInstructionByName;

TEST(HotProfileTest, ExposesBlockInstructionAndBranchHotness) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @branchy(i1 %cond) !prof !0 {
    entry:
      br i1 %cond, label %hot, label %cold, !prof !1
    hot:
      %hot_value = add i32 40, 2
      ret i32 %hot_value
    cold:
      %cold_value = add i32 1, 2
      ret i32 %cold_value
    }

    !0 = !{!"function_entry_count", i64 100}
    !1 = !{!"branch_weights", i32 80, i32 20}
  )");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("branchy");
  ASSERT_NE(function, nullptr);
  auto *entry = &function->getEntryBlock();
  auto *hot = findBlock(function, "hot");
  auto *cold = findBlock(function, "cold");
  ASSERT_NE(hot, nullptr);
  ASSERT_NE(cold, nullptr);
  auto *hot_value = findInstructionByName(function, "hot_value");
  ASSERT_NE(hot_value, nullptr);

  ProfileAnalysisCache cache;
  Hot profile(
      *module,
      [&cache](llvm::Function &f) -> llvm::BlockFrequencyInfo & {
        return cache.getBFI(f);
      },
      [&cache](llvm::Function &f) -> llvm::BranchProbabilityInfo & {
        return cache.getBPI(f);
      });

  EXPECT_TRUE(profile.isAvailable());
  EXPECT_EQ(profile.getInvocations(function), 100U);
  EXPECT_EQ(profile.getInvocations(entry), 100U);
  EXPECT_EQ(profile.getInvocations(hot), 79U);
  EXPECT_EQ(profile.getInvocations(cold), 21U);
  EXPECT_EQ(profile.getInvocations(hot_value), 79U);
  EXPECT_NEAR(profile.getBranchFrequency(entry, hot), 0.8, 1e-9);
  EXPECT_NEAR(profile.getBranchFrequency(entry, cold), 0.2, 1e-9);
  EXPECT_EQ(profile.getSelfInstructions(function), profile.getTotalInstructions());
}

TEST(HotProfileTest, AccountsForDirectCalleeCostAtCallInstruction) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @callee(i32 %x) !prof !0 {
    entry:
      %a = add i32 %x, 1
      %b = add i32 %a, 2
      ret i32 %b
    }

    define i32 @caller(i32 %x) !prof !1 {
    entry:
      %call = call i32 @callee(i32 %x)
      ret i32 %call
    }

    !0 = !{!"function_entry_count", i64 10}
    !1 = !{!"function_entry_count", i64 10}
  )");
  ASSERT_NE(module, nullptr);

  auto *caller = module->getFunction("caller");
  auto *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  auto *call = findInstructionByName(caller, "call");
  ASSERT_NE(call, nullptr);

  ProfileAnalysisCache cache;
  Hot profile(
      *module,
      [&cache](llvm::Function &f) -> llvm::BlockFrequencyInfo & {
        return cache.getBFI(f);
      },
      [&cache](llvm::Function &f) -> llvm::BranchProbabilityInfo & {
        return cache.getBPI(f);
      });

  EXPECT_EQ(profile.getInvocations(caller), 10U);
  EXPECT_EQ(profile.getInvocations(callee), 10U);
  EXPECT_GT(profile.getTotalInstructions(call), profile.getSelfInstructions(call));
  EXPECT_GT(profile.getTotalInstructions(caller), profile.getSelfInstructions(caller));
}

TEST(HotProfileTest, ReportsLoopInvocationAndIterationEstimates) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @loop(i32 %n) !prof !0 {
    entry:
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %next, %body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit, !prof !1
    body:
      %next = add i32 %i, 1
      br label %header
    exit:
      ret void
    }

    !0 = !{!"function_entry_count", i64 10}
    !1 = !{!"branch_weights", i32 90, i32 10}
  )");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("loop");
  ASSERT_NE(function, nullptr);

  ProfileAnalysisCache cache;
  Hot profile(
      *module,
      [&cache](llvm::Function &f) -> llvm::BlockFrequencyInfo & {
        return cache.getBFI(f);
      },
      [&cache](llvm::Function &f) -> llvm::BranchProbabilityInfo & {
        return cache.getBPI(f);
      });

  auto &loop_info = cache.getLoopInfo(*function);
  ASSERT_EQ(loop_info.getLoopsInPreorder().size(), 1U);
  auto *loop = loop_info.getLoopsInPreorder().front();

  EXPECT_TRUE(profile.hasBeenExecuted(loop));
  EXPECT_GT(profile.getIterations(loop), 0U);
  EXPECT_GT(profile.getAverageLoopIterationsPerInvocation(loop), 0.0);
  EXPECT_GT(profile.getAverageTotalInstructionsPerIteration(loop), 0.0);
}

} // namespace
