#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "Analysis/Loop/SCCDAGAttrs.h"
#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::GenericSCC;
using lotus::analysis::loop::LoopDependenceEdgeKind;
using lotus::analysis::loop::StackObjectClonableSCC;
using lotus::unittest::findInstructionByName;
using lotus::unittest::findPhi;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopClassificationTest : public ::testing::Test {
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

TEST_F(LoopClassificationTest, ClassifiesIVAndReductionSCCsAndTripCount) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @classify(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %latch ]
      %cmp = icmp slt i32 %i, 8
      br i1 %cmp, label %body, label %exit

    body:
      %contrib = add i32 %i, %n
      %sum.next = add i32 %sum, %contrib
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("classify");
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
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);
  analyses.materializeSCCAttrs(DT, PDT);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(content->doesHaveCompileTimeKnownTripCount());
  EXPECT_GT(content->getCompileTimeTripCount(), 0u);
  ASSERT_TRUE(content->hasSCCAttrs());

  auto *attrs = content->getSCCAttrs();
  auto *sumPhi = findPhi(function, "sum");
  auto *iPhi = findPhi(function, "i");
  ASSERT_NE(sumPhi, nullptr);
  ASSERT_NE(iPhi, nullptr);
  auto *sumSCC = content->getSCCDAG()->getSCC(sumPhi);
  auto *iSCC = content->getSCCDAG()->getSCC(iPhi);
  ASSERT_NE(sumSCC, nullptr);
  ASSERT_NE(iSCC, nullptr);

  auto *sumInfo = attrs->getSCCAttrs(sumSCC);
  auto *iInfo = attrs->getSCCAttrs(iSCC);
  ASSERT_NE(sumInfo, nullptr);
  ASSERT_NE(iInfo, nullptr);
  EXPECT_EQ(sumInfo->getKind(), GenericSCC::BINARY_REDUCTION);
  EXPECT_EQ(iInfo->getKind(), GenericSCC::LINEAR_INDUCTION_VARIABLE);

  auto reductionSCCs = attrs->getSCCsOfKind(GenericSCC::BINARY_REDUCTION);
  EXPECT_EQ(reductionSCCs.size(), 1u);
  EXPECT_EQ((*reductionSCCs.begin())->getSCC(), sumSCC);

  auto carriedSCCs = attrs->getSCCsWithLoopCarriedDependencies();
  EXPECT_FALSE(carriedSCCs.empty());
  auto carriedDataSCCs = attrs->getSCCsWithLoopCarriedDataDependencies();
  EXPECT_FALSE(carriedDataSCCs.empty());
  auto carriedControlSCCs = attrs->getSCCsWithLoopCarriedControlDependencies();
  for (auto *controlSCC : carriedControlSCCs) {
    EXPECT_NE(carriedSCCs.find(controlSCC), carriedSCCs.end());
  }

  EXPECT_TRUE(attrs->isLoopGovernedBySCC(iSCC));
  EXPECT_FALSE(attrs->isLoopGovernedBySCC(sumSCC));

  auto liveOutsNotReducible =
      attrs->getLiveOutVariablesThatAreNotReducable(content->getEnvironment());
  EXPECT_TRUE(liveOutsNotReducible.empty());
}

TEST_F(LoopClassificationTest,
       KeepsClonableStackObjectClassifiedForLaterTransformation) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @clonable(i32 %n) {
    entry:
      %slot = alloca i32, align 4
      store i32 0, i32* %slot, align 4
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %ld = load i32, i32* %slot, align 4
      %inc = add i32 %ld, 1
      store i32 %inc, i32* %slot, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("clonable");
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
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);
  analyses.materializeSCCAttrs(DT, PDT);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  auto *attrs = content->getSCCAttrs();
  ASSERT_NE(attrs, nullptr);
  auto *ld = findInstructionByName(function, "ld");
  ASSERT_NE(ld, nullptr);
  auto *scc = content->getSCCDAG()->getSCC(ld);
  ASSERT_NE(scc, nullptr);
  auto *info = attrs->getSCCAttrs(scc);
  ASSERT_NE(info, nullptr);
  EXPECT_TRUE(info->getKind() == GenericSCC::STACK_OBJECT_CLONABLE ||
              info->getKind() == GenericSCC::LOOP_CARRIED_UNKNOWN);
  if (info->getKind() == GenericSCC::STACK_OBJECT_CLONABLE) {
    auto *clonableInfo = dynamic_cast<StackObjectClonableSCC *>(info);
    ASSERT_NE(clonableInfo, nullptr);
    EXPECT_FALSE(clonableInfo->getMemoryLocationsToClone().empty());
  }

  auto carriedSCCs = attrs->getSCCsWithLoopCarriedDependencies();
  EXPECT_NE(std::find_if(carriedSCCs.begin(),
                         carriedSCCs.end(),
                         [scc](auto *carried) { return carried->getSCC() == scc; }),
            carriedSCCs.end());
}

TEST_F(LoopClassificationTest,
       ExposesRetainedClonableObjectMetadataForStackObjectSCCs) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @clonable_init(i32 %n) {
    entry:
      %slot = alloca i32, align 4
      store i32 42, i32* %slot, align 4
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %ld = load i32, i32* %slot, align 4
      %inc = add i32 %ld, 1
      store i32 %inc, i32* %slot, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("clonable_init");
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
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);
  analyses.materializeSCCAttrs(DT, PDT);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *attrs = content->getSCCAttrs();
  ASSERT_NE(attrs, nullptr);

  auto *ld = findInstructionByName(function, "ld");
  ASSERT_NE(ld, nullptr);
  auto *scc = content->getSCCDAG()->getSCC(ld);
  ASSERT_NE(scc, nullptr);
  auto *info = attrs->getSCCAttrs(scc);
  ASSERT_NE(info, nullptr);
  ASSERT_TRUE(info->getKind() == GenericSCC::STACK_OBJECT_CLONABLE ||
              info->getKind() == GenericSCC::LOOP_CARRIED_UNKNOWN);
  if (info->getKind() == GenericSCC::STACK_OBJECT_CLONABLE) {
    auto *clonableInfo = dynamic_cast<StackObjectClonableSCC *>(info);
    ASSERT_NE(clonableInfo, nullptr);
    EXPECT_FALSE(clonableInfo->getClonableMemoryObjects().empty());
    for (auto *location : clonableInfo->getClonableMemoryObjects()) {
      ASSERT_NE(location, nullptr);
      EXPECT_NE(location->getAllocation(), nullptr);
    }
  }
}

TEST_F(LoopClassificationTest,
       ClassifiesPeriodicSCCAndMarksItNonReducibleForLiveOuts) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @periodic_toggle(i32 %n) {
    entry:
      br label %header

    header:
      %flip = phi i32 [ 0, %entry ], [ %next, %latch ]
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %next = xor i32 %flip, 1
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %flip
    }
  )");
  auto *function = module->getFunction("periodic_toggle");
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
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);
  analyses.materializeSCCAttrs(DT, PDT);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  auto *attrs = content->getSCCAttrs();
  ASSERT_NE(attrs, nullptr);

  auto *flipPhi = findPhi(function, "flip");
  ASSERT_NE(flipPhi, nullptr);
  auto *flipSCC = content->getSCCDAG()->getSCC(flipPhi);
  ASSERT_NE(flipSCC, nullptr);

  auto *info = attrs->getSCCAttrs(flipSCC);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->getKind(), GenericSCC::PERIODIC_VARIABLE);

  auto liveOutsNotReducible =
      attrs->getLiveOutVariablesThatAreNotReducable(content->getEnvironment());
  EXPECT_FALSE(liveOutsNotReducible.empty());
}

TEST_F(LoopClassificationTest, ComputesIgnoredSCCDAGParentsThroughLoopIteration) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @ignored_sccdag(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %inc = add i32 %i, 1
      %sum.next = add i32 %sum, %inc
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("ignored_sccdag");
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
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);
  analyses.materializeSCCAttrs(DT, PDT);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  auto *attrs = content->getSCCAttrs();
  ASSERT_NE(attrs, nullptr);

  auto *sumPhi = findPhi(function, "sum");
  auto *iPhi = findPhi(function, "i");
  ASSERT_NE(sumPhi, nullptr);
  ASSERT_NE(iPhi, nullptr);

  auto *sumSCC = content->getSCCDAG()->getSCC(sumPhi);
  auto *iSCC = content->getSCCDAG()->getSCC(iPhi);
  ASSERT_NE(sumSCC, nullptr);
  ASSERT_NE(iSCC, nullptr);

  auto ignored = attrs->computeSCCDAGWhenSCCsAreIgnored(
      [](GenericSCC *info) {
        return info != nullptr && info->getKind() == GenericSCC::LOOP_ITERATION;
      });
  auto &parents = ignored.first;
  ASSERT_NE(parents.find(sumSCC), parents.end());
  EXPECT_NE(parents.at(sumSCC).find(iSCC), parents.at(sumSCC).end());
}

} // namespace
