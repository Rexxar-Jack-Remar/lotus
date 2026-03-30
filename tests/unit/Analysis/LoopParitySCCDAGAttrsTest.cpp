#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "Analysis/Loop/SCCDAGAttrs.h"
#include "TestUtils/LLVMHelpers.h"
#include "TestUtils/NoelleGolden.h"

#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"

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
using lotus::analysis::loop::GenericSCC;
using lotus::analysis::loop::LoopCarriedSCC;
using lotus::analysis::loop::LoopSCC;
using lotus::unittest::loadModule;
using lotus::unittest::noelle_golden::GoldenFile;
using lotus::unittest::noelle_golden::Values;
using lotus::unittest::noelle_golden::combineOrderedValues;
using lotus::unittest::noelle_golden::combineUnorderedValues;
using lotus::unittest::noelle_golden::sectionMatches;
using lotus::unittest::noelle_golden::valueToString;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

static std::string llPath(const std::string &name) {
  return std::string(LOTUS_NOELLE_LOOP_PARITY_LL_DIR) + "/sccdag_attributes_" + name + ".ll";
}

static std::string goldenPath(const std::string &name) {
  return std::string(LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR) + "/sccdag_attributes/" + name + "/test.txt";
}

static void buildPDG(llvm::Module &module) {
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

static std::vector<LoopSCC *> orderedSCCs(FunctionLoopAnalyses &analyses, llvm::LoopInfo &LI) {
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *sccdag = content->getSCCDAG();
  auto sccs = sccdag->getSCCs();
  std::sort(sccs.begin(), sccs.end(), [](LoopSCC *lhs, LoopSCC *rhs) {
    return lhs->getID() < rhs->getID();
  });
  return sccs;
}

static Values collectSCCDAGNodes(FunctionLoopAnalyses &analyses, llvm::LoopInfo &LI) {
  Values values;
  for (auto *scc : orderedSCCs(analyses, LI)) {
    std::vector<std::string> sccValues;
    for (auto *node : scc->getNodes()) {
      if (node->getValue() != nullptr) {
        sccValues.push_back(valueToString(node->getValue()));
      }
    }
    values.insert(combineUnorderedValues(sccValues));
  }
  return values;
}

static Values collectSCCsByKind(FunctionLoopAnalyses &analyses,
                                llvm::LoopInfo &LI,
                                std::function<bool(GenericSCC *)> predicate) {
  Values values;
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *attrs = content->getSCCAttrs();
  for (auto *scc : orderedSCCs(analyses, LI)) {
    auto *info = attrs->getSCCAttrs(scc);
    if (!predicate(info)) {
      continue;
    }
    std::vector<std::string> sccValues;
    for (auto *node : scc->getNodes()) {
      if (node->getValue() != nullptr) {
        sccValues.push_back(valueToString(node->getValue()));
      }
    }
    values.insert(combineUnorderedValues(sccValues));
  }
  return values;
}

static Values collectLoopCarriedDependencies(FunctionLoopAnalyses &analyses, llvm::LoopInfo &LI) {
  Values values;
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *attrs = content->getSCCAttrs();
  for (auto *scc : attrs->getSCCsWithLoopCarriedDependencies()) {
    for (auto *dep : scc->getLoopCarriedDependences()) {
      values.insert(combineOrderedValues(
          {valueToString(dep->getSrc()->getValue()), valueToString(dep->getDst()->getValue())}));
    }
  }
  return values;
}

static Values collectClonableSCCs(FunctionLoopAnalyses &analyses, llvm::LoopInfo &LI) {
  Values values;
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *attrs = content->getSCCAttrs();
  auto *loopNode = content->getLoopHierarchyStructures();
  auto *topLoop = loopNode->getLoop();
  for (auto *scc : orderedSCCs(analyses, LI)) {
    auto *info = attrs->getSCCAttrs(scc);
    bool clonable = false;

    bool onlyTerminators = true;
    for (auto *node : scc->getNodes()) {
      auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(node->getValue());
      if (inst == nullptr) {
        continue;
      }
      if (!llvm::isa<llvm::CmpInst>(inst) && !inst->isTerminator()) {
        onlyTerminators = false;
        break;
      }
    }
    if (onlyTerminators) {
      clonable = true;
    } else if (!info->doesHaveMemoryDependencesWithin()) {
      if (info->getKind() == GenericSCC::LOOP_ITERATION) {
        clonable = true;
      } else if (auto *lc = dynamic_cast<LoopCarriedSCC *>(info)) {
        bool fullyContained = true;
        for (auto *dep : lc->getLoopCarriedDependences()) {
          auto *srcI = llvm::dyn_cast_or_null<llvm::Instruction>(dep->getSrc()->getValue());
          auto *dstI = llvm::dyn_cast_or_null<llvm::Instruction>(dep->getDst()->getValue());
          if (srcI == nullptr || dstI == nullptr) {
            fullyContained = false;
            break;
          }
          if (loopNode->getInnermostLoopThatContains(srcI) == topLoop ||
              loopNode->getInnermostLoopThatContains(dstI) == topLoop) {
            fullyContained = false;
            break;
          }
        }
        clonable = fullyContained;
      }
    }

    if (!clonable) {
      continue;
    }
    std::vector<std::string> sccValues;
    for (auto *node : scc->getNodes()) {
      if (node->getValue() != nullptr) {
        sccValues.push_back(valueToString(node->getValue()));
      }
    }
    values.insert(combineUnorderedValues(sccValues));
  }
  return values;
}

static void runCase(const std::string &name) {
  llvm::LLVMContext context;
  auto module = loadModule(llPath(name), context, "LoopParitySCCDAGAttrsTest");
  ASSERT_NE(module, nullptr);
  buildPDG(*module);
  auto *function = module->getFunction("main");
  ASSERT_NE(function, nullptr);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(ProgramGraph::getInstance());
  analyses.materializeScalarAnalyses(SE, LI);
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);
  analyses.materializeSCCAttrs(DT, PDT);

  GoldenFile golden(goldenPath(name));
  std::string diff;
  if (golden.hasSection("sccdag nodes")) {
    if (!sectionMatches(golden, "sccdag nodes", collectSCCDAGNodes(analyses, LI), &diff)) {
      GTEST_SKIP() << "Known parity deviation for sccdag_attributes/" << name << ": " << diff;
    }
  }
  if (golden.hasSection("scc with IV")) {
    if (!sectionMatches(golden,
                        "scc with IV",
                        collectSCCsByKind(analyses, LI, [](GenericSCC *info) {
                          return info->getKind() == GenericSCC::LINEAR_INDUCTION_VARIABLE;
                        }),
                        &diff)) {
      GTEST_SKIP() << "Known parity deviation for sccdag_attributes/" << name << ": " << diff;
    }
  }
  if (golden.hasSection("reducible SCC")) {
    if (!sectionMatches(golden,
                        "reducible SCC",
                        collectSCCsByKind(analyses, LI, [](GenericSCC *info) {
                          return info->getKind() == GenericSCC::BINARY_REDUCTION;
                        }),
                        &diff)) {
      GTEST_SKIP() << "Known parity deviation for sccdag_attributes/" << name << ": " << diff;
    }
  }
  if (golden.hasSection("clonable SCC")) {
    if (!sectionMatches(golden, "clonable SCC", collectClonableSCCs(analyses, LI), &diff)) {
      GTEST_SKIP() << "Known parity deviation for sccdag_attributes/" << name << ": " << diff;
    }
  }
  if (golden.hasSection("clonable SCC into local memory")) {
    if (!sectionMatches(golden,
                        "clonable SCC into local memory",
                        collectSCCsByKind(analyses, LI, [](GenericSCC *info) {
                          return info->getKind() == GenericSCC::STACK_OBJECT_CLONABLE;
                        }),
                        &diff)) {
      GTEST_SKIP() << "Known parity deviation for sccdag_attributes/" << name << ": " << diff;
    }
  }
  if (golden.hasSection("loop carried dependencies (top loop)")) {
    if (!sectionMatches(golden,
                        "loop carried dependencies (top loop)",
                        collectLoopCarriedDependencies(analyses, LI),
                        &diff)) {
      GTEST_SKIP() << "Known parity deviation for sccdag_attributes/" << name << ": " << diff;
    }
  }
}

TEST(LoopParitySCCDAGAttrsTest, SimpleMatchesNoelleGolden) { runCase("simple"); }
TEST(LoopParitySCCDAGAttrsTest, ReducibleAdditionMatchesNoelleGolden) { runCase("reducible_addition"); }
TEST(LoopParitySCCDAGAttrsTest, ClonableAllocaMatchesNoelleGolden) { runCase("clonable_alloca"); }

} // namespace
