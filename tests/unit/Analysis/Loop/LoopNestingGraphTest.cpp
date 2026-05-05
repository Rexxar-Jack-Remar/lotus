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
using lotus::analysis::loop::LoopNestingGraph;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModuleChecked;

static std::unique_ptr<FunctionLoopAnalyses>
buildAnalysesForLoopingFunction(llvm::Function &F) {
  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(F);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(F);
  if (LI.empty()) {
    return nullptr;
  }
  return std::make_unique<FunctionLoopAnalyses>(F, LI, DT, PDT);
}

TEST(LoopNestingGraphTest, BuildsReachableDirectCallEdgesFromEntry) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @callee(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      br label %latch

    latch:
      %inc = add i32 %i, 1
      br label %header

    exit:
      ret i32 %i
    }

    define i32 @dead(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      br label %latch

    latch:
      %inc = add i32 %i, 1
      br label %header

    exit:
      ret i32 %i
    }

    define i32 @main(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %call = call i32 @callee(i32 %n)
      br label %latch

    latch:
      %inc = add i32 %i, 1
      br label %header

    exit:
      ret i32 %i
    }
  )");

  auto *mainFunction = module->getFunction("main");
  auto *calleeFunction = module->getFunction("callee");
  auto *deadFunction = module->getFunction("dead");
  ASSERT_NE(mainFunction, nullptr);
  ASSERT_NE(calleeFunction, nullptr);
  ASSERT_NE(deadFunction, nullptr);

  auto mainAnalyses = buildAnalysesForLoopingFunction(*mainFunction);
  auto calleeAnalyses = buildAnalysesForLoopingFunction(*calleeFunction);
  auto deadAnalyses = buildAnalysesForLoopingFunction(*deadFunction);
  ASSERT_NE(mainAnalyses, nullptr);
  ASSERT_NE(calleeAnalyses, nullptr);
  ASSERT_NE(deadAnalyses, nullptr);

  std::vector<FunctionLoopAnalyses *> analyses = {
      mainAnalyses.get(), calleeAnalyses.get(), deadAnalyses.get()};
  auto graph =
      LoopNestingGraph::buildFromAnalyses(analyses, *module, mainFunction);
  ASSERT_NE(graph, nullptr);

  auto *mainLoop = mainAnalyses->getLoopStructures().front();
  auto *calleeLoop = calleeAnalyses->getLoopStructures().front();
  auto *deadLoop = deadAnalyses->getLoopStructures().front();
  ASSERT_NE(mainLoop, nullptr);
  ASSERT_NE(calleeLoop, nullptr);
  ASSERT_NE(deadLoop, nullptr);

  auto *mainNode = graph->getLoopNode(mainLoop);
  auto *calleeNode = graph->getLoopNode(calleeLoop);
  EXPECT_NE(mainNode, nullptr);
  EXPECT_NE(calleeNode, nullptr);
  EXPECT_EQ(graph->getLoopNode(deadLoop), nullptr);

  auto *callInst =
      llvm::dyn_cast_or_null<llvm::CallBase>(findInstructionByName(mainFunction,
                                                                   "call"));
  ASSERT_NE(callInst, nullptr);
  auto *edge = mainNode->getNestingEdgeTo(calleeNode);
  ASSERT_NE(edge, nullptr);
  EXPECT_TRUE(edge->isMust());
  ASSERT_EQ(edge->getSubEdges().size(), 1u);
  EXPECT_EQ(edge->getSubEdges().front()->getInstructionNode()->getInstruction(),
            callInst);

  auto *entryNode = graph->getEntryNode(mainFunction);
  ASSERT_NE(entryNode, nullptr);
  EXPECT_EQ(entryNode->getLoop(), mainLoop);
}

TEST(LoopNestingGraphTest, CreatesMayEdgesForIndirectCallsAndMergesSubedges) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @callee_a(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      br label %latch

    latch:
      %inc = add i32 %i, 1
      br label %header

    exit:
      ret i32 %i
    }

    define i32 @callee_b(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      br label %latch

    latch:
      %inc = add i32 %i, 1
      br label %header

    exit:
      ret i32 %i
    }

    define i32 @main(i32 %n, i32 (i32)* %fp) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %direct = call i32 @callee_a(i32 %n)
      %indirect = call i32 %fp(i32 %n)
      br label %latch

    latch:
      %inc = add i32 %i, 1
      br label %header

    exit:
      ret i32 %i
    }
  )");

  auto *mainFunction = module->getFunction("main");
  auto *calleeAFunction = module->getFunction("callee_a");
  auto *calleeBFunction = module->getFunction("callee_b");
  ASSERT_NE(mainFunction, nullptr);
  ASSERT_NE(calleeAFunction, nullptr);
  ASSERT_NE(calleeBFunction, nullptr);

  auto mainAnalyses = buildAnalysesForLoopingFunction(*mainFunction);
  auto calleeAAnalyses = buildAnalysesForLoopingFunction(*calleeAFunction);
  auto calleeBAnalyses = buildAnalysesForLoopingFunction(*calleeBFunction);
  ASSERT_NE(mainAnalyses, nullptr);
  ASSERT_NE(calleeAAnalyses, nullptr);
  ASSERT_NE(calleeBAnalyses, nullptr);

  std::vector<FunctionLoopAnalyses *> analyses = {
      mainAnalyses.get(), calleeAAnalyses.get(), calleeBAnalyses.get()};
  auto graph =
      LoopNestingGraph::buildFromAnalyses(analyses, *module, mainFunction);
  ASSERT_NE(graph, nullptr);

  auto *mainLoop = mainAnalyses->getLoopStructures().front();
  auto *calleeALoop = calleeAAnalyses->getLoopStructures().front();
  auto *calleeBLoop = calleeBAnalyses->getLoopStructures().front();
  ASSERT_NE(mainLoop, nullptr);
  ASSERT_NE(calleeALoop, nullptr);
  ASSERT_NE(calleeBLoop, nullptr);

  auto *mainNode = graph->getLoopNode(mainLoop);
  auto *calleeANode = graph->getLoopNode(calleeALoop);
  auto *calleeBNode = graph->getLoopNode(calleeBLoop);
  ASSERT_NE(mainNode, nullptr);
  ASSERT_NE(calleeANode, nullptr);
  ASSERT_NE(calleeBNode, nullptr);

  auto *directCall =
      llvm::dyn_cast_or_null<llvm::CallBase>(findInstructionByName(mainFunction,
                                                                   "direct"));
  auto *indirectCall =
      llvm::dyn_cast_or_null<llvm::CallBase>(findInstructionByName(mainFunction,
                                                                   "indirect"));
  ASSERT_NE(directCall, nullptr);
  ASSERT_NE(indirectCall, nullptr);

  auto *directEdge = mainNode->getNestingEdgeTo(calleeANode);
  ASSERT_NE(directEdge, nullptr);
  EXPECT_TRUE(directEdge->isMust());
  ASSERT_EQ(directEdge->getSubEdges().size(), 2u);

  bool sawDirectSubedge = false;
  bool sawIndirectMaySubedgeToA = false;
  for (auto *subEdge : directEdge->getSubEdges()) {
    auto *instructionNode = subEdge->getInstructionNode();
    ASSERT_NE(instructionNode, nullptr);
    if (instructionNode->getInstruction() == directCall) {
      sawDirectSubedge = true;
      EXPECT_TRUE(subEdge->isMust());
    }
    if (instructionNode->getInstruction() == indirectCall) {
      sawIndirectMaySubedgeToA = true;
      EXPECT_FALSE(subEdge->isMust());
    }
  }
  EXPECT_TRUE(sawDirectSubedge);
  EXPECT_TRUE(sawIndirectMaySubedgeToA);

  auto *indirectEdge = mainNode->getNestingEdgeTo(calleeBNode);
  ASSERT_NE(indirectEdge, nullptr);
  EXPECT_FALSE(indirectEdge->isMust());
  ASSERT_EQ(indirectEdge->getSubEdges().size(), 1u);
  ASSERT_NE(indirectEdge->getSubEdges().front()->getInstructionNode(), nullptr);
  EXPECT_EQ(indirectEdge->getSubEdges().front()->getInstructionNode()->getInstruction(),
            indirectCall);
  EXPECT_FALSE(indirectEdge->getSubEdges().front()->isMust());
}

} // namespace
