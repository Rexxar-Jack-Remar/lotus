#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "Analysis/Loop/LoopCarriedDependencies.h"
#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::LoopCarriedDependencies;
using lotus::analysis::loop::LoopDependenceEdgeKind;
using lotus::unittest::findInstructionByName;
using lotus::unittest::findPhi;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopEnvironmentAndCarriedDepsTest : public ::testing::Test {
protected:
  void SetUp() override { graph.reset(); }
  void TearDown() override { graph.reset(); }

  void buildPDG(llvm::Module &module) {
    auto &registry = *llvm::PassRegistry::getPassRegistry();
    llvm::initializeCore(registry);
    llvm::initializeAnalysis(registry);
    llvm::initializeTransformUtils(registry);

    llvm::legacy::PassManager pm;
    pm.add(new DataDependencyGraph());
    pm.add(new ControlDependencyGraph());
    pm.add(new ProgramDependencyGraph());
    pm.run(module);
  }

  ProgramGraph &graph = ProgramGraph::getInstance();
};

TEST_F(LoopEnvironmentAndCarriedDepsTest, BuildsEnvironmentAndMarksCarriedEdges) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @loop_env(i32* %p, i32 %n) {
    entry:
      %limit = add i32 %n, 4
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %latch ]
      %cmp = icmp slt i32 %i, %limit
      br i1 %cmp, label %body, label %exit

    body:
      %ld = load i32, i32* %p, align 4
      %sum.next = add i32 %sum, %ld
      store i32 %sum.next, i32* %p, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("loop_env");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(content->hasEnvironment());

  auto *env = content->getEnvironment();
  EXPECT_GE(env->getNumberOfLiveIns(), 1u);
  EXPECT_GE(env->getNumberOfLiveOuts(), 1u);

  auto *limit = findInstructionByName(function, "limit");
  ASSERT_NE(limit, nullptr);
  EXPECT_TRUE(env->isLiveIn(limit));

  auto *sumPhi = findPhi(function, "sum");
  ASSERT_NE(sumPhi, nullptr);
  EXPECT_TRUE(env->isProducer(sumPhi));

  auto *graphView = content->getLoopDependenceGraph();
  ASSERT_NE(graphView, nullptr);

  bool hasCarriedVariable = false;
  bool hasCarriedMemory = false;
  for (auto *edge : graphView->getEdges()) {
    if (!edge->isLoopCarried()) {
      continue;
    }
    if (edge->getKind() == LoopDependenceEdgeKind::Variable
        && edge->getDst()->getValue() == sumPhi) {
      hasCarriedVariable = true;
    }
    if (edge->getKind() == LoopDependenceEdgeKind::Memory) {
      hasCarriedMemory = true;
    }
  }

  EXPECT_TRUE(hasCarriedVariable);
  EXPECT_TRUE(hasCarriedMemory);

  auto carriedForLoop = LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(
      *content->getLoopStructure(),
      content->getLoopHierarchyStructures(),
      *graphView);
  EXPECT_FALSE(carriedForLoop.empty());
}

} // namespace
