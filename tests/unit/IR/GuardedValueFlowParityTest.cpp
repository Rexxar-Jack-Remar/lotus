#include "IR/GuardedValueFlow/GuardedValueFlowBuilder.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace llvm::gvg;

namespace {

class GuardedValueFlowParityTest : public ::testing::Test {
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
      err.print("GuardedValueFlowParityTest", errs());
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

TEST_F(GuardedValueFlowParityTest, ModelsReturnPhiSelectAndOperationalSites) {
  const char *source = R"(
    define i32 @test(i1 %cond, i32* %p, i32 %x, i32 %y, i32 %idx) {
    entry:
      %sum = add i32 %x, %y
      %cmp = icmp sgt i32 %sum, 0
      br i1 %cond, label %then, label %else
    then:
      %div = sdiv i32 %sum, %y
      %gep = getelementptr i32, i32* %p, i32 %idx
      store i32 %div, i32* %gep
      br label %merge
    else:
      %sel = select i1 %cmp, i32 %x, i32 %y
      br label %merge
    merge:
      %phi = phi i32 [ %div, %then ], [ %sel, %else ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  GuardedValueFlowReturnNode *common_return = nullptr;
  GuardedValueFlowPhiNode *phi_node = nullptr;
  GuardedValueFlowNode *select_node = nullptr;
  StoreInst *store_inst = nullptr;
  for (const auto &node_ptr : graph.nodes()) {
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::CommonReturn)
      common_return = dyn_cast<GuardedValueFlowReturnNode>(node_ptr.get());
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::Phi)
      phi_node = dyn_cast<GuardedValueFlowPhiNode>(node_ptr.get());
  }

  for (Instruction &I : instructions(*F)) {
    if (isa<SelectInst>(&I))
      select_node = graph.findNode(&I);
    if (auto *SI = dyn_cast<StoreInst>(&I))
      store_inst = SI;
  }

  ASSERT_NE(common_return, nullptr);
  ASSERT_NE(phi_node, nullptr);
  ASSERT_NE(select_node, nullptr);
  ASSERT_EQ(common_return->children().size(), 1u);
  EXPECT_EQ(common_return->children().front().target, phi_node);
  EXPECT_NE(common_return->getReturnSite(phi_node), nullptr);
  EXPECT_EQ(common_return->getRegion(), graph.findRegion(&F->getEntryBlock()));

  ASSERT_EQ(phi_node->incoming().size(), 2u);
  EXPECT_NE(phi_node->incoming()[0].value_node, nullptr);
  EXPECT_NE(phi_node->incoming()[1].value_node, nullptr);
  EXPECT_NE(phi_node->incoming()[0].incoming_block, nullptr);
  EXPECT_NE(phi_node->incoming()[1].incoming_block, nullptr);

  ASSERT_EQ(select_node->children().size(), 1u);
  auto *select_opcode =
      dyn_cast<GuardedValueFlowOpcodeNode>(select_node->children().front().target);
  ASSERT_NE(select_opcode, nullptr);
  EXPECT_EQ(select_opcode->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::Select);
  EXPECT_EQ(select_opcode->children().size(), 3u);
  ASSERT_NE(store_inst, nullptr);
  auto *store_mem = graph.findStoreMemoryNode(store_inst->getValueOperand(), store_inst);
  ASSERT_NE(store_mem, nullptr);
  EXPECT_EQ(store_mem->getRegion(), graph.findRegion(store_inst->getParent()));
  EXPECT_EQ(select_node->getRegion(), graph.findRegion(select_node->getParentBasicBlock()));
  EXPECT_EQ(graph.findNode(F->getArg(0))->getRegion(),
            graph.findRegion(&F->getEntryBlock()));

  bool saw_compare_site = false;
  bool saw_div_site = false;
  bool saw_gep_site = false;
  for (const auto &site_ptr : graph.sites()) {
    if (auto *cmp_site =
            dynamic_cast<GuardedValueFlowCompareSite *>(site_ptr.get())) {
      saw_compare_site = true;
      EXPECT_NE(cmp_site->getLhsOperand(), nullptr);
      EXPECT_NE(cmp_site->getRhsOperand(), nullptr);
    }
    if (auto *div_site = dynamic_cast<GuardedValueFlowDivSite *>(site_ptr.get())) {
      saw_div_site = true;
      EXPECT_NE(div_site->getLhsOperand(), nullptr);
      EXPECT_NE(div_site->getRhsOperand(), nullptr);
    }
    if (auto *gep_site =
            dynamic_cast<GuardedValueFlowGEPReferenceSite *>(site_ptr.get())) {
      saw_gep_site = true;
      EXPECT_NE(gep_site->getPointerOperand(), nullptr);
      EXPECT_FALSE(gep_site->getOffsetOperands().empty());
      EXPECT_NE(gep_site->getResultNode(), nullptr);
    }
  }

  EXPECT_TRUE(saw_compare_site);
  EXPECT_TRUE(saw_div_site);
  EXPECT_TRUE(saw_gep_site);
}

TEST_F(GuardedValueFlowParityTest, BuildsCompoundRegionsForNestedControlDependence) {
  const char *source = R"(
    define void @test(i1 %a, i1 %b, i32* %p) {
    entry:
      br i1 %a, label %outer, label %exit
    outer:
      br i1 %b, label %inner, label %join
    inner:
      store i32 1, i32* %p
      br label %join
    join:
      ret void
    exit:
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

  BasicBlock *outer = F->getBasicBlockList().getNextNode(F->getEntryBlock());
  ASSERT_NE(outer, nullptr);
  BasicBlock *inner = nullptr;
  for (BasicBlock &BB : *F) {
    if (BB.getName() == "inner")
      inner = &BB;
  }
  ASSERT_NE(inner, nullptr);

  auto *outer_region = graph.findRegion(outer);
  auto *inner_region = graph.findRegion(inner);
  ASSERT_NE(outer_region, nullptr);
  ASSERT_NE(inner_region, nullptr);
  EXPECT_FALSE(outer_region->isAlwaysTrue());
  EXPECT_TRUE(inner_region->isCompound());
  EXPECT_EQ(inner_region->children().size(), 2u);
}

TEST_F(GuardedValueFlowParityTest,
       PreservesLabeledConditionForNonImmediateControlledBlock) {
  const char *source = R"(
    define void @test(i1 %a, i32* %p) {
    entry:
      br i1 %a, label %then, label %exit
    then:
      br label %mid
    mid:
      br label %inner
    inner:
      store i32 1, i32* %p
      ret void
    exit:
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

  BasicBlock *then_bb = nullptr;
  BasicBlock *inner_bb = nullptr;
  for (BasicBlock &BB : *F) {
    if (BB.getName() == "then")
      then_bb = &BB;
    if (BB.getName() == "inner")
      inner_bb = &BB;
  }
  ASSERT_NE(then_bb, nullptr);
  ASSERT_NE(inner_bb, nullptr);

  auto block_conditions = graph.getBlockConditions(inner_bb);
  ASSERT_EQ(block_conditions.size(), 1u);
  EXPECT_EQ(block_conditions.front().control_block, &F->getEntryBlock());
  EXPECT_EQ(block_conditions.front().guard_successor, then_bb);
  EXPECT_TRUE(block_conditions.front().sense);
  EXPECT_NE(block_conditions.front().condition_node, nullptr);
  EXPECT_EQ(block_conditions.front().condition.getKind(),
            ConditionRef::Kind::StructuralGuard);
}

TEST_F(GuardedValueFlowParityTest,
       ModelsConstantExprAndConstantAggregateOperands) {
  const char *source = R"(
    @g = global i32 0

    define i8* @test() {
    entry:
      %slot = alloca [2 x i32]
      store [2 x i32] [i32 1, i32 2], [2 x i32]* %slot
      ret i8* bitcast (i32* @g to i8*)
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  StoreInst *store = nullptr;
  ReturnInst *ret = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *SI = dyn_cast<StoreInst>(&I))
      store = SI;
    if (auto *RI = dyn_cast<ReturnInst>(&I))
      ret = RI;
  }
  ASSERT_NE(store, nullptr);
  ASSERT_NE(ret, nullptr);

  auto *aggregate_node = graph.findNode(store->getValueOperand());
  ASSERT_NE(aggregate_node, nullptr);
  ASSERT_EQ(aggregate_node->children().size(), 1u);
  auto *concat =
      dyn_cast<GuardedValueFlowOpcodeNode>(aggregate_node->children().front().target);
  ASSERT_NE(concat, nullptr);
  EXPECT_EQ(concat->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::Concat);

  auto *const_expr_node = graph.findNode(ret->getReturnValue());
  ASSERT_NE(const_expr_node, nullptr);
  ASSERT_EQ(const_expr_node->children().size(), 1u);
  auto *cast_node =
      dyn_cast<GuardedValueFlowOpcodeNode>(const_expr_node->children().front().target);
  ASSERT_NE(cast_node, nullptr);
  EXPECT_EQ(cast_node->getOpcodeKind(),
            GuardedValueFlowOpcodeNode::OpcodeKind::BitCast);
}

TEST_F(GuardedValueFlowParityTest, RejectsUnsupportedSwitchFunctions) {
  const char *source = R"(
    define i32 @test(i32 %x) {
    entry:
      switch i32 %x, label %default [ i32 1, label %one ]
    one:
      ret i32 1
    default:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  EXPECT_FALSE(pipeline.builder->hasGraphFor(*F));
}

TEST_F(GuardedValueFlowParityTest, RejectsUnsupportedFNegFunctions) {
  const char *source = R"(
    define float @test(float %x) {
    entry:
      %neg = fneg float %x
      ret float %neg
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  EXPECT_FALSE(pipeline.builder->hasGraphFor(*F));
}

} // namespace
