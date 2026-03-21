#include "IR/GuardedValueFlow/GuardedValueFlowBuilder.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace llvm::gvg;

namespace {

class GuardedValueFlowTest : public ::testing::Test {
protected:
  LLVMContext context_;

  struct Pipeline {
    std::unique_ptr<legacy::PassManager> pm;
    GuardedValueFlowGraphBuilderPass *builder{nullptr};
  };

  static void initializePassInfra() {
    static bool initialized = false;
    if (initialized)
      return;

    auto &registry = *PassRegistry::getPassRegistry();
    initializeCore(registry);
    initializeAnalysis(registry);
    initializeTransformUtils(registry);
    initialized = true;
  }

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context_);
    if (!module)
      err.print("GuardedValueFlowTest", errs());
    return module;
  }

  Pipeline runBuilder(Module &M) {
    initializePassInfra();
    Pipeline pipeline;
    pipeline.pm = std::make_unique<legacy::PassManager>();
    pipeline.builder = new GuardedValueFlowGraphBuilderPass();
    pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
    pipeline.pm->add(new gsa::GateAnalysisPass());
    pipeline.pm->add(pipeline.builder);
    pipeline.pm->run(M);
    return pipeline;
  }
};

TEST_F(GuardedValueFlowTest, BuildsCallLoadStoreAndRegionNodes) {
  const char *source = R"(
    define i32* @callee(i32** %p) {
    entry:
      %tmp = load i32*, i32** %p
      ret i32* %tmp
    }

    define i32* @test(i1 %cond, i32** %p, i32* %v) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32* %v, i32** %p
      br label %merge
    else:
      %ret = call i32* @callee(i32** %p)
      br label %merge
    merge:
      %phi = phi i32* [ %v, %then ], [ %ret, %else ]
      ret i32* %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  EXPECT_NE(graph.findRegion(&F->getEntryBlock()), nullptr);
  EXPECT_NE(graph.findNode(F->getArg(1)), nullptr);

  Instruction *call_inst = nullptr;
  Instruction *phi_inst = nullptr;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      if (isa<CallInst>(&I))
        call_inst = &I;
      if (isa<PHINode>(&I))
        phi_inst = &I;
    }
  }

  ASSERT_NE(call_inst, nullptr);
  ASSERT_NE(phi_inst, nullptr);
  EXPECT_NE(graph.findCallSite(call_inst), nullptr);

  GuardedValueFlowNode *phi_node = graph.findNode(phi_inst);
  ASSERT_NE(phi_node, nullptr);
  EXPECT_EQ(phi_node->getKind(), GuardedValueFlowNode::Kind::Phi);
  EXPECT_EQ(phi_node->children().size(), 2u);
}

} // namespace
