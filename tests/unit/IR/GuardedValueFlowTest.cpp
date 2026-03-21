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

TEST_F(GuardedValueFlowTest, UsesDensePseudoInterfaceIndices) {
  const char *source = R"(
    define void @test() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());
  auto *site = graph.createSite<GuardedValueFlowCallSite>(&graph, ret_inst);

  auto *pseudo_input0 = graph.createNode<GuardedValueFlowCallOutputNode>(
      GuardedValueFlowNode::Kind::CallSitePseudoInput,
      Type::getInt32Ty(context_), &graph, &F->getEntryBlock(), nullptr, ret_inst,
      F);
  pseudo_input0->setIndex(0);
  site->addPseudoInput(F, pseudo_input0);

  auto *pseudo_input2 = graph.createNode<GuardedValueFlowCallOutputNode>(
      GuardedValueFlowNode::Kind::CallSitePseudoInput,
      Type::getInt32Ty(context_), &graph, &F->getEntryBlock(), nullptr, ret_inst,
      F);
  pseudo_input2->setIndex(2);
  site->addPseudoInput(F, pseudo_input2);

  EXPECT_EQ(site->getNumPseudoInputs(F), 2u);
  EXPECT_EQ(site->getPseudoInput(F, 0), pseudo_input0);
  EXPECT_EQ(site->getPseudoInput(F, 1), pseudo_input2);
  EXPECT_EQ(site->getPseudoInput(F, 2), nullptr);
  EXPECT_EQ(pseudo_input0->getIndex(), 0u);
  EXPECT_EQ(pseudo_input2->getIndex(), 1u);
}

TEST_F(GuardedValueFlowTest,
       KeepsAnonymousStoreMemoryProducersDistinctForMatchingRegions) {
  const char *source = R"(
    define void @test() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  GuardedValueFlowGraph graph(F);
  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());
  auto *load_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt32Ty(context_), &graph,
      &F->getEntryBlock(), nullptr, ret_inst);
  auto *producer_a = graph.createAnonymousStoreMemoryNode(
      Type::getInt32Ty(context_), &F->getEntryBlock(), ret_inst, "anon.a");
  auto *producer_b = graph.createAnonymousStoreMemoryNode(
      Type::getInt32Ty(context_), &F->getEntryBlock(), ret_inst, "anon.b");

  auto *cond_node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt1Ty(context_),
      &graph, &F->getEntryBlock());
  auto *region_true = graph.findOrCreateUnitRegion(cond_node, true,
                                                   &F->getEntryBlock(),
                                                   ConditionRef::none());
  auto *region_false = graph.findOrCreateUnitRegion(cond_node, false,
                                                    &F->getEntryBlock(),
                                                    ConditionRef::none());

  ASSERT_NE(producer_a, producer_b);
  load_mem->addMatchingRegion(producer_a, region_true, ConditionRef::none());
  load_mem->addMatchingRegion(producer_b, region_false, ConditionRef::none());

  ASSERT_EQ(load_mem->getMatchingRegions().size(), 2u);
  EXPECT_EQ(load_mem->getMatchingRegion(producer_a), region_true);
  EXPECT_EQ(load_mem->getMatchingRegion(producer_b), region_false);
}

TEST_F(GuardedValueFlowTest, StoresSummaryNodesPerCalleeWithoutOverwrite) {
  const char *source = R"(
    declare void @callee_a()
    declare void @callee_b()

    define void @test() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  Function *callee_a = module->getFunction("callee_a");
  Function *callee_b = module->getFunction("callee_b");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(callee_a, nullptr);
  ASSERT_NE(callee_b, nullptr);

  GuardedValueFlowGraph graph(F);
  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());
  auto *site = graph.createSite<GuardedValueFlowCallSite>(&graph, ret_inst);

  auto *input_a = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteArgumentSummary,
      Type::getInt32Ty(context_), &graph, &F->getEntryBlock(), ret_inst,
      callee_a, 1);
  auto *input_b = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteArgumentSummary,
      Type::getInt32Ty(context_), &graph, &F->getEntryBlock(), ret_inst,
      callee_b, 1);
  auto *output_a = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary,
      Type::getInt32Ty(context_), &graph, &F->getEntryBlock(), ret_inst,
      callee_a, 2);
  auto *output_b = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary,
      Type::getInt32Ty(context_), &graph, &F->getEntryBlock(), ret_inst,
      callee_b, 2);

  site->setInputSummaryNode(callee_a, 1, input_a);
  site->setInputSummaryNode(callee_b, 1, input_b);
  site->setOutputSummaryNode(callee_a, 2, output_a);
  site->setOutputSummaryNode(callee_b, 2, output_b);

  EXPECT_EQ(site->getInputSummaryNode(callee_a, 1), input_a);
  EXPECT_EQ(site->getInputSummaryNode(callee_b, 1), input_b);
  EXPECT_EQ(site->getOutputSummaryNode(callee_a, 2), output_a);
  EXPECT_EQ(site->getOutputSummaryNode(callee_b, 2), output_b);
}

TEST_F(GuardedValueFlowTest, PreservesImportedSemanticConditionIdentity) {
  const char *source = R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *callee = module->getFunction("callee");
  Function *caller = module->getFunction("caller");
  ASSERT_NE(callee, nullptr);
  ASSERT_NE(caller, nullptr);

  GuardedValueFlowGraph callee_graph(callee);
  GuardedValueFlowGraph caller_graph(caller);
  auto *source_cond = PathCond::createBlockAtom(&callee->getEntryBlock());
  auto *callee_region = callee_graph.findOrCreateSemanticRegion(
      source_cond, &callee->getEntryBlock());
  ASSERT_NE(callee_region, nullptr);
  ASSERT_NE(callee_region->getConditionNode(), nullptr);

  auto *imported_cond = PathCond::createImportedAtom(caller, source_cond);
  auto *caller_region = caller_graph.findOrCreateSemanticRegion(
      imported_cond, &caller->getEntryBlock(), callee_region->getConditionNode());
  ASSERT_NE(caller_region, nullptr);
  EXPECT_TRUE(caller_region->isInterfaceRegion());
  EXPECT_EQ(caller_region->getConditionNode(), callee_region->getConditionNode());
  EXPECT_TRUE(caller_region->children().empty());
  EXPECT_EQ(caller_region->getConditionNode()->getGraph(), &callee_graph);
  EXPECT_EQ(caller_region->getConditionNode()->getRegion(), callee_region);
}

} // namespace
