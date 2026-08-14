#include "Analysis/CFG/CFGReachability.h"
#include "Analysis/CFG/CodeMetrics.h"
#include "Analysis/CFG/DominatorForest.h"
#include "Analysis/CFG/TopologicalOrder.h"
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

  CFGReachability reachability(*function);
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

  CFGReachability reachability(*function);
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

  CFGReachability reachability(*function);
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
  CFGReachability reachability(*f);
  BasicBlock *foreign = &g->getEntryBlock();

  EXPECT_DEATH((void)reachability.reachable(foreign, foreign),
               "block not found");

  Instruction *foreignInstruction = foreign->getTerminator();
  EXPECT_DEATH(
      (void)reachability.reachable(foreignInstruction, foreignInstruction),
      "outside the analyzed function");
}
#endif

TEST(TopologicalOrderTest, DeclarationProducesEmptyOrder) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @decl()

    define void @defined() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  TopologicalOrder order;
  EXPECT_FALSE(order.runOnFunction(*module->getFunction("defined")));
  EXPECT_NE(order.begin(), order.end());

  EXPECT_FALSE(order.runOnFunction(*module->getFunction("decl")));
  EXPECT_EQ(order.begin(), order.end());
}

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

    define i32 @two_returns(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      ret i32 1
    right:
      ret i32 2
    }

    define void @spin_forever() {
    entry:
      br label %entry
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
  EXPECT_EQ(calcCyclomaticComplexity(*module->getFunction("two_returns")), 2u);
  EXPECT_EQ(calcCyclomaticComplexity(*module->getFunction("spin_forever")), 1u);
  EXPECT_EQ(calcCyclomaticComplexity(*module->getFunction("with_unreachable")),
            1u);
}

TEST(CodeMetricsTest, NPathEndsOnlyAtRealCFGTerminals) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @decl()

    define void @single() {
    entry:
      ret void
    }

    define void @two_returns(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      ret void
    right:
      ret void
    }

    define void @spin_forever() {
    entry:
      br label %entry
    }

    define void @loop_with_exit(i1 %done) {
    entry:
      br label %loop
    loop:
      br i1 %done, label %exit, label %loop
    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  EXPECT_EQ(nPath(*module->getFunction("decl")), 0u);
  EXPECT_EQ(nPath(*module->getFunction("single")), 1u);
  EXPECT_EQ(nPath(*module->getFunction("two_returns")), 2u);
  EXPECT_EQ(nPath(*module->getFunction("spin_forever")), 0u);
  EXPECT_EQ(nPath(*module->getFunction("loop_with_exit")), 1u);
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

TEST(DominatorForestTest, SubsetPreservesNearestRetainedAncestor) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @f() {
    entry:
      br label %middle
    middle:
      br label %exit
    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("f");
  auto blockIt = function->begin();
  BasicBlock *entry = &*blockIt++;
  BasicBlock *middle = &*blockIt++;
  BasicBlock *exit = &*blockIt++;
  ASSERT_EQ(blockIt, function->end());

  std::set<BasicBlock *> retained{entry, exit};

  DominatorTree dt(*function);
  noelle::DominatorForest fullDominators(dt);
  noelle::DominatorForest subsetDominators(fullDominators, retained);

  EXPECT_EQ(subsetDominators.getNode(middle), nullptr);
  ASSERT_NE(subsetDominators.getNode(entry), nullptr);
  ASSERT_NE(subsetDominators.getNode(exit), nullptr);
  EXPECT_TRUE(subsetDominators.dominates(entry, exit));
  EXPECT_EQ(subsetDominators.getNode(exit)->getParent(),
            subsetDominators.getNode(entry));
  EXPECT_EQ(subsetDominators.getNode(entry)->getLevel(), 0u);
  EXPECT_EQ(subsetDominators.getNode(exit)->getLevel(), 1u);

  PostDominatorTree pdt;
  pdt.recalculate(*function);
  noelle::DominatorForest fullPostDominators(pdt);
  noelle::DominatorForest subsetPostDominators(fullPostDominators, retained);

  EXPECT_TRUE(subsetPostDominators.dominates(exit, entry));
  EXPECT_EQ(subsetPostDominators.getNode(entry)->getParent(),
            subsetPostDominators.getNode(exit));
  EXPECT_EQ(subsetPostDominators.getNode(exit)->getLevel(), 0u);
  EXPECT_EQ(subsetPostDominators.getNode(entry)->getLevel(), 1u);
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
