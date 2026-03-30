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
#include "llvm/Passes/PassBuilder.h"
#include "llvm/PassRegistry.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::unittest::findInstructionByName;
using lotus::unittest::findPhi;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopScalarAnalysisTest : public ::testing::Test {
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

TEST_F(LoopScalarAnalysisTest, MaterializesInvariantsAndInductionVariables) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @loop_scalar(i32 %n) {
    entry:
      %limit = add i32 %n, 5
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %latch ]
      %cmp = icmp slt i32 %i, %limit
      br i1 %cmp, label %body, label %exit

    body:
      %tmp = add i32 %i, 1
      %sum.next = add i32 %sum, %tmp
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("loop_scalar");
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

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(content->hasInvariantManager());
  ASSERT_TRUE(content->hasInductionVariableManager());

  auto *invariants = content->getInvariantManager();
  auto *ivs = content->getInductionVariableManager();

  auto *limit = findInstructionByName(function, "limit");
  auto *cmp = findInstructionByName(function, "cmp");
  auto *tmp = findInstructionByName(function, "tmp");
  auto *iPhi = findPhi(function, "i");
  auto *sumPhi = findPhi(function, "sum");
  ASSERT_NE(limit, nullptr);
  ASSERT_NE(cmp, nullptr);
  ASSERT_NE(tmp, nullptr);
  ASSERT_NE(iPhi, nullptr);
  ASSERT_NE(sumPhi, nullptr);

  EXPECT_TRUE(invariants->isLoopInvariant(limit));
  EXPECT_FALSE(invariants->isLoopInvariant(cmp));
  EXPECT_FALSE(invariants->isLoopInvariant(tmp));

  auto detectedIVs = ivs->getInductionVariables(*content->getLoopStructure());
  ASSERT_EQ(detectedIVs.size(), 1u);
  auto *iv = *detectedIVs.begin();
  ASSERT_NE(iv, nullptr);
  EXPECT_EQ(iv->getLoopEntryPHI(), iPhi);
  EXPECT_NE(iv->getStartValue(), nullptr);
  EXPECT_NE(iv->getSingleComputedStepValue(), nullptr);
  EXPECT_TRUE(iv->isIVInstruction(iPhi));
  EXPECT_FALSE(iv->isIVInstruction(sumPhi));

  auto *governing = ivs->getLoopGoverningInductionVariable(
      *content->getLoopStructure());
  ASSERT_NE(governing, nullptr);
  EXPECT_TRUE(governing->isSCCContainingIVWellFormed());
  EXPECT_EQ(governing->getInductionVariable(), iv);
  EXPECT_EQ(governing->getHeaderCompareInstructionToComputeExitCondition(), cmp);
  EXPECT_EQ(governing->getExitConditionValue(), limit);
}

} // namespace
