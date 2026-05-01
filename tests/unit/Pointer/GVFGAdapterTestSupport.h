#pragma once

#include "TestUtils/LLVMHelpers.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#define private public
#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAdapter.h"
#undef private

using namespace llvm;
using namespace lotus::gvfg;
using namespace lotus::unittest;

namespace {

static bool containsChild(GuardedValueFlowNode *node, GuardedValueFlowNode *child) {
  if (!node || !child)
    return false;
  for (const auto &edge : node->children()) {
    if (edge.target == child)
      return true;
  }
  return false;
}

static bool hasChildKind(GuardedValueFlowNode *node,
                         GuardedValueFlowNode::Kind kind) {
  if (!node)
    return false;
  for (const auto &edge : node->children()) {
    if (edge.target && edge.target->getKind() == kind)
      return true;
  }
  return false;
}

static bool hasDescendantKind(GuardedValueFlowNode *node,
                              GuardedValueFlowNode::Kind kind,
                              unsigned depth = 3) {
  if (!node)
    return false;
  for (const auto &edge : node->children()) {
    if (!edge.target)
      continue;
    if (edge.target->getKind() == kind)
      return true;
    if (depth > 0 && hasDescendantKind(edge.target, kind, depth - 1))
      return true;
  }
  return false;
}

static bool containsDescendantNode(GuardedValueFlowNode *node,
                                   GuardedValueFlowNode *target,
                                   unsigned depth = 3) {
  if (!node || !target)
    return false;
  for (const auto &edge : node->children()) {
    if (!edge.target)
      continue;
    if (edge.target == target)
      return true;
    if (depth > 0 && containsDescendantNode(edge.target, target, depth - 1))
      return true;
  }
  return false;
}

void initializePassInfra() {
  static bool initialized = false;
  if (initialized)
    return;

  auto &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  initialized = true;
}

struct PipelineResult {
  std::unique_ptr<legacy::PassManager> pm;
  LotusAA *lotus{nullptr};
  GuardedValueFlowGraphBuilderPass *builder{nullptr};
};

PipelineResult runPipeline(Module &M) {
  initializePassInfra();

  PipelineResult result;
  result.pm = std::make_unique<legacy::PassManager>();
  result.lotus = new LotusAA();
  result.builder = new GuardedValueFlowGraphBuilderPass();

  result.pm->add(new gsa::ControlDependenceAnalysisPass());
  result.pm->add(new gsa::GateAnalysisPass());
  result.pm->add(result.lotus);
  result.pm->add(result.builder);
  result.pm->add(new LotusGuardedValueFlowAdapterPass());
  result.pm->run(M);
  return result;
}

PipelineResult runPipelineWithoutAdapter(Module &M) {
  initializePassInfra();

  PipelineResult result;
  result.pm = std::make_unique<legacy::PassManager>();
  result.lotus = new LotusAA();
  result.builder = new GuardedValueFlowGraphBuilderPass();

  result.pm->add(new gsa::ControlDependenceAnalysisPass());
  result.pm->add(new gsa::GateAnalysisPass());
  result.pm->add(result.lotus);
  result.pm->add(result.builder);
  result.pm->run(M);
  return result;
}





















} // namespace
