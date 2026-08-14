#include "Analysis/CFG/CFGReachability.h"
#include "Analysis/CFG/CodeMetrics.h"
#include "Analysis/CFG/DominatorForest.h"
#include "TestUtils/LLVMHelpers.h"

#include <unordered_map>

#include <gtest/gtest.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>

using namespace llvm;
using namespace lotus::unittest;

namespace {

TEST(CFGReachabilityTest, AcyclicBlockDoesNotReachEarlierInstruction) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @f() {
    entry:
      %a = add i32 1, 2
      %b = add i32 %a, 3
      ret i32 %b
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("f");
  Instruction *a = findInstructionByName(*function, "a");
  Instruction *b = findInstructionByName(*function, "b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  CFGReachability reachability(function);
  EXPECT_TRUE(reachability.reachable(a, b));
  EXPECT_FALSE(reachability.reachable(b, a));
}

TEST(CFGReachabilityTest, CycleReachesEarlierInstructionInSameBlock) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @f(i1 %again) {
    loop:
      %a = add i32 1, 2
      %b = add i32 %a, 3
      br i1 %again, label %loop, label %exit
    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("f");
  Instruction *a = findInstructionByName(*function, "a");
  Instruction *b = findInstructionByName(*function, "b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  CFGReachability reachability(function);
  EXPECT_TRUE(reachability.reachable(b, a));
}

TEST(CFGReachabilityTest, MultiBlockCycleReachesEarlierInstruction) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @f(i1 %again) {
    header:
      %a = add i32 1, 2
      %b = add i32 %a, 3
      br label %latch
    latch:
      br i1 %again, label %header, label %exit
    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("f");
  Instruction *a = findInstructionByName(*function, "a");
  Instruction *b = findInstructionByName(*function, "b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  CFGReachability reachability(function);
  EXPECT_TRUE(reachability.reachable(b, a));
}

#ifndef NDEBUG
TEST(CFGReachabilityTest, RejectsReflexiveQueryForForeignBlock) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @f() { entry: ret void }
    define void @g() { entry: ret void }
  )");
  ASSERT_NE(module, nullptr);

  Function *f = module->getFunction("f");
  Function *g = module->getFunction("g");
  CFGReachability reachability(f);
  BasicBlock *foreign = &g->getEntryBlock();

  EXPECT_DEATH((void)reachability.reachable(foreign, foreign),
               "block not found");
}
#endif

TEST(CodeMetricsTest, CyclomaticComplexityUsesEntryReachableCFG) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @decl()

    define void @single() {
    entry:
      ret void
    }

    define void @linear() {
    entry:
      br label %next
    next:
      ret void
    }

    define void @conditional(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      br label %exit
    right:
      br label %exit
    exit:
      ret void
    }

    define void @with_unreachable() {
    entry:
      ret void
    dead:
      br i1 undef, label %dead_left, label %dead_right
    dead_left:
      ret void
    dead_right:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  EXPECT_EQ(calcCyclomaticComplexity(*module->getFunction("decl")), 0u);
  EXPECT_EQ(calcCyclomaticComplexity(*module->getFunction("single")), 1u);
  EXPECT_EQ(calcCyclomaticComplexity(*module->getFunction("linear")), 1u);
  EXPECT_EQ(calcCyclomaticComplexity(*module->getFunction("conditional")), 2u);
  EXPECT_EQ(calcCyclomaticComplexity(*module->getFunction("with_unreachable")),
            1u);
}

TEST(DominatorForestTest, InstructionDominanceIsReflexiveForDTAndPDT) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @f() {
    entry:
      %a = add i32 1, 2
      %b = add i32 %a, 3
      ret i32 %b
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("f");
  Instruction *a = findInstructionByName(*function, "a");
  Instruction *b = findInstructionByName(*function, "b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  DominatorTree dt(*function);
  PostDominatorTree pdt;
  pdt.recalculate(*function);
  noelle::DominatorForest dominators(dt);
  noelle::DominatorForest postDominators(pdt);

  EXPECT_TRUE(dominators.dominates(a, a));
  EXPECT_TRUE(postDominators.dominates(a, a));
  EXPECT_TRUE(dominators.dominates(a, b));
  EXPECT_FALSE(dominators.dominates(b, a));
  EXPECT_TRUE(postDominators.dominates(b, a));
  EXPECT_FALSE(postDominators.dominates(a, b));
}

TEST(DominatorForestTest, TransferToClonesRebuildsLookupAndSkipsVirtualRoot) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @original(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      ret void
    right:
      ret void
    }

    define void @clone(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      ret void
    right:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *original = module->getFunction("original");
  Function *clone = module->getFunction("clone");
  PostDominatorTree pdt;
  pdt.recalculate(*original);
  noelle::DominatorForest forest(pdt);

  std::unordered_map<BasicBlock *, BasicBlock *> cloneMap;
  auto cloneIt = clone->begin();
  for (BasicBlock &block : *original) {
    ASSERT_NE(cloneIt, clone->end());
    cloneMap[&block] = &*cloneIt++;
  }

  BasicBlock *oldEntry = &original->getEntryBlock();
  BasicBlock *newEntry = &clone->getEntryBlock();
  ASSERT_NE(forest.getNode(oldEntry), nullptr);

  forest.transferToClones(cloneMap);

  EXPECT_EQ(forest.getNode(oldEntry), nullptr);
  ASSERT_NE(forest.getNode(newEntry), nullptr);
  EXPECT_EQ(forest.getNode(newEntry)->getBlock(), newEntry);
}

} // namespace
