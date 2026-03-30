#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Passes/PassBuilder.h"

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "Analysis/Loop/LoopNestingGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::FunctionLoopAnalysesPass;
using lotus::analysis::loop::LoopNestingGraph;
using lotus::analysis::loop::LoopStructure;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModuleChecked;

std::unique_ptr<llvm::Module> makeLoopModule(llvm::LLVMContext &context) {
  return parseModuleChecked(context, R"(
    define void @nested(i32 %n, i32 %m) {
    entry:
      br label %outer.header

    outer.header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %outer.latch ]
      %outer.cmp = icmp slt i32 %i, %n
      br i1 %outer.cmp, label %outer.body, label %exit

    outer.body:
      br label %inner.header

    inner.header:
      %j = phi i32 [ 0, %outer.body ], [ %j.next, %inner.latch ]
      %inner.cmp = icmp slt i32 %j, %m
      br i1 %inner.cmp, label %inner.body, label %inner.exit

    inner.body:
      %inner.work = add i32 %i, %j
      br label %inner.latch

    inner.latch:
      %j.next = add i32 %j, 1
      br label %inner.header

    inner.exit:
      br label %outer.latch

    outer.latch:
      %i.next = add i32 %i, 1
      br label %outer.header

    exit:
      ret void
    }

    define void @siblings(i32 %n) {
    entry:
      br label %first.header

    first.header:
      %a = phi i32 [ 0, %entry ], [ %a.next, %first.latch ]
      %first.cmp = icmp slt i32 %a, %n
      br i1 %first.cmp, label %first.body, label %between

    first.body:
      br label %first.latch

    first.latch:
      %a.next = add i32 %a, 1
      br label %first.header

    between:
      br label %second.header

    second.header:
      %b = phi i32 [ 0, %between ], [ %b.next, %second.latch ]
      %second.cmp = icmp slt i32 %b, %n
      br i1 %second.cmp, label %second.body, label %done

    second.body:
      br label %second.latch

    second.latch:
      %b.next = add i32 %b, 1
      br label %second.header

    done:
      ret void
    }
  )");
}

TEST(FunctionLoopAnalysesTest, BuildsNestedForestAndContentLookup) {
  llvm::LLVMContext context;
  auto module = makeLoopModule(context);
  auto *function = module->getFunction("nested");
  ASSERT_NE(function, nullptr);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  auto structures = analyses.getLoopStructures();
  ASSERT_EQ(structures.size(), 2u);
  ASSERT_NE(analyses.getLoopForest(), nullptr);
  EXPECT_EQ(analyses.getLoopForest()->getNumberOfLoops(), 2u);

  auto trees = analyses.getLoopForest()->getTrees();
  ASSERT_EQ(trees.size(), 1u);
  auto *root = *trees.begin();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getLoop()->getHeader()->getName(), "outer.header");
  ASSERT_EQ(root->getChildren().size(), 1u);

  auto *innerWork = findInstructionByName(function, "inner.work");
  ASSERT_NE(innerWork, nullptr);
  LoopStructure *innermost = analyses.getLoopForest()
                                 ->getInnermostLoopThatContains(innerWork)
                                 ->getLoop();
  ASSERT_NE(innermost, nullptr);
  EXPECT_EQ(innermost->getHeader()->getName(), "inner.header");

  llvm::BasicBlock *exitBlock = nullptr;
  for (auto &bb : *function) {
    if (bb.getName() == "exit") {
      exitBlock = &bb;
      break;
    }
  }
  ASSERT_NE(exitBlock, nullptr);
  auto *exitTerminator = exitBlock->getTerminator();
  ASSERT_NE(exitTerminator, nullptr);
  EXPECT_EQ(
      analyses.getLoopForest()->getInnermostLoopThatContains(exitTerminator),
      nullptr);

  auto *content =
      analyses.getLoopContent(*LI.getLoopFor(structures[0]->getHeader()));
  ASSERT_NE(content, nullptr);
  EXPECT_EQ(content->getLoopStructure()->getHeader()->getName(),
            "outer.header");
  EXPECT_EQ(
      content->getNestedMostLoopStructure(innerWork)->getHeader()->getName(),
      "inner.header");
}

TEST(FunctionLoopAnalysesTest, PreservesSiblingTopLevelLoopsAsSeparateTrees) {
  llvm::LLVMContext context;
  auto module = makeLoopModule(context);
  auto *function = module->getFunction("siblings");
  ASSERT_NE(function, nullptr);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  auto *forest = analyses.getLoopForest();
  ASSERT_NE(forest, nullptr);
  EXPECT_EQ(forest->getNumberOfLoops(), 2u);
  EXPECT_EQ(forest->getTrees().size(), 2u);
}

TEST(FunctionLoopAnalysesTest, RegistersAsNewPmFunctionAnalysis) {
  llvm::LLVMContext context;
  auto module = makeLoopModule(context);
  auto *function = module->getFunction("nested");
  ASSERT_NE(function, nullptr);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  FAM.registerPass([&] { return FunctionLoopAnalysesPass(); });

  auto &result = FAM.getResult<FunctionLoopAnalysesPass>(*function);
  auto structures = result.getLoopStructures();
  ASSERT_EQ(structures.size(), 2u);
  EXPECT_EQ(result.getLoopForest()->getTrees().size(), 1u);
}

TEST(FunctionLoopAnalysesTest,
     BuildsInterproceduralLoopNestingGraphWithMustAndMayEdges) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @must_callee(i32 %n) {
    entry:
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      br label %latch
    latch:
      %i.next = add i32 %i, 1
      br label %header
    exit:
      ret void
    }

    define void @may_callee(i32 %n) {
    entry:
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      br label %latch
    latch:
      %i.next = add i32 %i, 1
      br label %header
    exit:
      ret void
    }

    define void @driver(i32 %n) {
    entry:
      br label %header
    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      call void @must_callee(i32 %n)
      %fp = select i1 true, void (i32)* @may_callee, void (i32)* @must_callee
      call void %fp(i32 %n)
      br label %latch
    latch:
      %i.next = add i32 %i, 1
      br label %header
    exit:
      ret void
    }
  )");

  std::vector<std::unique_ptr<FunctionLoopAnalyses>> ownedAnalyses;
  std::vector<FunctionLoopAnalyses *> analyses;
  for (auto &function : *module) {
    if (function.isDeclaration()) {
      continue;
    }
    llvm::DominatorTree DT(function);
    llvm::PostDominatorTree PDT;
    PDT.recalculate(function);
    llvm::LoopInfo LI(DT);
    if (LI.empty()) {
      continue;
    }
    ownedAnalyses.emplace_back(new FunctionLoopAnalyses(function, LI, DT, PDT));
    analyses.push_back(ownedAnalyses.back().get());
  }

  auto *entryFunction = module->getFunction("driver");
  ASSERT_NE(entryFunction, nullptr);
  auto graph =
      LoopNestingGraph::buildFromAnalyses(analyses, *module, entryFunction);
  ASSERT_NE(graph, nullptr);

  LoopStructure *driverLoop = nullptr;
  LoopStructure *mustLoop = nullptr;
  LoopStructure *mayLoop = nullptr;
  for (auto *analysis : analyses) {
    auto *function = analysis->getFunction();
    auto loops = analysis->getLoopStructures();
    ASSERT_EQ(loops.size(), 1u);
    if (function->getName() == "driver") {
      driverLoop = loops.front();
    } else if (function->getName() == "must_callee") {
      mustLoop = loops.front();
    } else if (function->getName() == "may_callee") {
      mayLoop = loops.front();
    }
  }

  ASSERT_NE(driverLoop, nullptr);
  ASSERT_NE(mustLoop, nullptr);
  ASSERT_NE(mayLoop, nullptr);

  auto *driverNode = graph->getLoopNode(driverLoop);
  auto *mustNode = graph->getLoopNode(mustLoop);
  auto *mayNode = graph->getLoopNode(mayLoop);
  ASSERT_NE(driverNode, nullptr);
  ASSERT_NE(mustNode, nullptr);
  ASSERT_NE(mayNode, nullptr);

  auto *mustEdge = driverNode->getNestingEdgeTo(mustNode);
  auto *mayEdge = driverNode->getNestingEdgeTo(mayNode);
  ASSERT_NE(mustEdge, nullptr);
  ASSERT_NE(mayEdge, nullptr);
  EXPECT_TRUE(mustEdge->isMust());
  EXPECT_FALSE(mayEdge->isMust());
}

} // namespace
