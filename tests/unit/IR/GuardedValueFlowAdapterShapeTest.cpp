#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "IR/GSA/GSA.h"
#include "IR/GuardedValueFlow/GuardedValueFlowBuilder.h"
#include "IR/GuardedValueFlow/LotusAdapter.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace llvm::gvg;

namespace {

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

std::unique_ptr<Module> parseModule(LLVMContext &Ctx, const char *IR) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("GuardedValueFlowAdapterShapeTest", errs());
  return M;
}

struct Pipeline {
  std::unique_ptr<legacy::PassManager> pm;
  GuardedValueFlowGraphBuilderPass *builder{nullptr};
};

Pipeline runPipeline(Module &M) {
  initializePassInfra();
  Pipeline pipeline;
  pipeline.pm = std::make_unique<legacy::PassManager>();
  pipeline.builder = new GuardedValueFlowGraphBuilderPass();
  pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
  pipeline.pm->add(new gsa::GateAnalysisPass());
  pipeline.pm->add(new LotusAA());
  pipeline.pm->add(pipeline.builder);
  pipeline.pm->add(new LotusGuardedValueFlowAdapterPass());
  pipeline.pm->run(M);
  return pipeline;
}

TEST(GuardedValueFlowAdapterShape, BuildsMemoryAndPseudoInterfaceShape) {
  const char *IR = R"(
    define i32* @callee(i1 %cond, i32** %p, i32* %a, i32* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32* %a, i32** %p
      br label %merge
    else:
      store i32* %b, i32** %p
      br label %merge
    merge:
      %v = load i32*, i32** %p
      ret i32* %v
    }

    define i32* @test(i1 %cond, i32** %p, i32* %a, i32* %b) {
    entry:
      %ret = call i32* @callee(i1 %cond, i32** %p, i32* %a, i32* %b)
      ret i32* %ret
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);
  ASSERT_EQ(site->getCallees().size(), 1u);

  auto *common_output = site->getCommonOutput();
  ASSERT_NE(common_output, nullptr);
  EXPECT_EQ(common_output->getKind(),
            GuardedValueFlowNode::Kind::CallSiteCommonOutput);

  size_t load_mem_count = 0;
  size_t store_mem_count = 0;
  size_t path_cond_match_count = 0;
  size_t pseudo_interface_count = 0;
  for (const auto &node_ptr : graph.nodes()) {
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::LoadMemory)
      ++load_mem_count;
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::StoreMemory)
      ++store_mem_count;
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::CallSitePseudoInput ||
        node_ptr->getKind() == GuardedValueFlowNode::Kind::CallSitePseudoOutput)
      ++pseudo_interface_count;
    for (const auto &match : node_ptr->getMatchingConditions()) {
      if (match.second.getKind() == ConditionRef::Kind::SemanticPathCond)
        ++path_cond_match_count;
    }
  }

  EXPECT_GT(load_mem_count, 0u);
  EXPECT_GT(store_mem_count, 0u);
  EXPECT_GT(path_cond_match_count, 0u);
  EXPECT_GE(pseudo_interface_count, 0u);
}

} // namespace
