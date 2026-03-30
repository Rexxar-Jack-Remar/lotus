#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopIterationSpaceTest : public ::testing::Test {
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

TEST_F(LoopIterationSpaceTest, ClearsCarriedMemoryEdgesForAffineSelfRecurrence) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @array_loop(i32* %base, i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %idx = sext i32 %i to i64
      %ptr = getelementptr inbounds i32, i32* %base, i64 %idx
      %ld = load i32, i32* %ptr, align 4
      store i32 %ld, i32* %ptr, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("array_loop");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI);
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  auto *iterationSpace = content->getLoopIterationSpaceAnalysis();
  ASSERT_NE(iterationSpace, nullptr);
  auto *ld = findInstructionByName(function, "ld");
  auto *store = lotus::unittest::findInstruction<llvm::StoreInst>(*function);
  ASSERT_NE(ld, nullptr);
  ASSERT_NE(store, nullptr);
  EXPECT_TRUE(
      iterationSpace->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
          ld, store));
}

} // namespace
