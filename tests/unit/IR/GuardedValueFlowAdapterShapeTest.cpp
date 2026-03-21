#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "IR/GSA/GSA.h"
#include "IR/GuardedValueFlow/GuardedValueFlowBuilder.h"
#include "IR/GuardedValueFlow/LotusAdapter.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace llvm::gvg;

namespace {

static bool containsUseSite(GuardedValueFlowNode *node,
                            GuardedValueFlowSite *site) {
  if (!node || !site)
    return false;
  for (GuardedValueFlowSite *use_site : node->useSites()) {
    if (use_site == site)
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
    define i32* @load_arg(i32** %p) {
    entry:
      %v = load i32*, i32** %p
      ret i32* %v
    }

    define void @store_arg(i32** %p, i32* %v) {
    entry:
      store i32* %v, i32** %p
      ret void
    }

    define void @test(i32** %p, i32* %v) {
    entry:
      %ret = call i32* @load_arg(i32** %p)
      call void @store_arg(i32** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  Function *load_callee = M->getFunction("load_arg");
  Function *store_callee = M->getFunction("store_arg");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(load_callee, nullptr);
  ASSERT_NE(store_callee, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*load_callee));
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*store_callee));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  CallBase *load_call = nullptr;
  CallBase *store_call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      Function *callee = CB->getCalledFunction();
      if (!callee)
        continue;
      if (callee->getName() == "load_arg")
        load_call = CB;
      else if (callee->getName() == "store_arg")
        store_call = CB;
    }
  }
  ASSERT_NE(load_call, nullptr);
  ASSERT_NE(store_call, nullptr);

  auto *load_site = graph.findCallSite(load_call);
  auto *store_site = graph.findCallSite(store_call);
  ASSERT_NE(load_site, nullptr);
  ASSERT_NE(store_site, nullptr);
  ASSERT_EQ(load_site->getCallees().size(), 1u);
  ASSERT_EQ(store_site->getCallees().size(), 1u);
  ASSERT_EQ(load_site->getCommonInputs().size(), 1u);
  ASSERT_EQ(store_site->getCommonInputs().size(), 2u);

  auto *common_output = load_site->getCommonOutput();
  ASSERT_NE(common_output, nullptr);
  EXPECT_EQ(common_output->getKind(),
            GuardedValueFlowNode::Kind::CallSiteCommonOutput);

  auto *caller_region = graph.findRegion(&F->getEntryBlock());
  ASSERT_NE(caller_region, nullptr);
  for (GuardedValueFlowNode *input : load_site->getCommonInputs()) {
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(containsUseSite(input, load_site));
    EXPECT_EQ(input->getRegion(), caller_region);
  }
  for (GuardedValueFlowNode *input : store_site->getCommonInputs()) {
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(containsUseSite(input, store_site));
    EXPECT_EQ(input->getRegion(), caller_region);
  }

  ASSERT_GT(load_site->getNumPseudoInputs(load_callee), 0u);
  auto *pseudo_input = load_site->getPseudoInput(load_callee, 0);
  ASSERT_NE(pseudo_input, nullptr);
  EXPECT_TRUE(containsUseSite(pseudo_input, load_site));
  EXPECT_EQ(pseudo_input->getRegion(), caller_region);
  EXPECT_EQ(pseudo_input->getIndex(), 0u);

  ASSERT_GT(store_site->getNumPseudoOutputs(store_callee), 0u);
  auto *pseudo_output = store_site->getPseudoOutput(store_callee, 0);
  ASSERT_NE(pseudo_output, nullptr);
  EXPECT_EQ(pseudo_output->getRegion(), caller_region);
  EXPECT_EQ(pseudo_output->children().size(), 1u);
  EXPECT_EQ(pseudo_output->children().front().target->getKind(),
            GuardedValueFlowNode::Kind::StoreMemory);
}

TEST(GuardedValueFlowAdapterShape, PreservesDistinctLoadMemoryNodesPerLoad) {
  const char *IR = R"(
    define i32 @test(i32* %p, i32 %v) {
    entry:
      store i32 %v, i32* %p
      %a = load i32, i32* %p
      %b = load i32, i32* %p
      %sum = add i32 %a, %b
      ret i32 %sum
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

  SmallVector<LoadInst *, 2> loads;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      loads.push_back(LI);
  }
  ASSERT_EQ(loads.size(), 2u);

  auto *first_load_mem = graph.findLoadMemoryNode(loads[0]);
  auto *second_load_mem = graph.findLoadMemoryNode(loads[1]);
  ASSERT_NE(first_load_mem, nullptr);
  ASSERT_NE(second_load_mem, nullptr);
  EXPECT_NE(first_load_mem, second_load_mem);

  auto *first_value_node = graph.findNode(loads[0]);
  auto *second_value_node = graph.findNode(loads[1]);
  ASSERT_NE(first_value_node, nullptr);
  ASSERT_NE(second_value_node, nullptr);
  ASSERT_EQ(first_value_node->children().size(), 1u);
  ASSERT_EQ(second_value_node->children().size(), 1u);
  EXPECT_EQ(first_value_node->children().front().target, first_load_mem);
  EXPECT_EQ(second_value_node->children().front().target, second_load_mem);

  EXPECT_FALSE(first_load_mem->getMatchingRegions().empty());
  EXPECT_FALSE(second_load_mem->getMatchingRegions().empty());
}

TEST(GuardedValueFlowAdapterShape, ModelsSemanticMatchingRegionsForConditionalLoad) {
  const char *IR = R"(
    define i32* @test(i1 %cond, i32** %p, i32* %a, i32* %b) {
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
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  LoadInst *load = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      load = LI;
      break;
    }
  }
  ASSERT_NE(load, nullptr);

  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_mem, nullptr);
  ASSERT_EQ(load_mem->getMatchingRegions().size(), 2u);

  SmallPtrSet<GuardedValueFlowRegionNode *, 4> matched_regions;
  for (const auto &match : load_mem->getMatchingRegions()) {
    EXPECT_NE(match.producer, nullptr);
    EXPECT_NE(match.region, nullptr);
    EXPECT_EQ(match.provenance.getKind(), ConditionRef::Kind::SemanticPathCond);
    EXPECT_TRUE(match.region->isInterfaceRegion());
    ASSERT_NE(match.region->getConditionNode(), nullptr);
    EXPECT_EQ(match.region->getConditionNode()->getRegion(), match.region);
    EXPECT_EQ(match.region->getInterfacePathCondition(),
              match.provenance.getPathCond());
    matched_regions.insert(match.region);
  }
  EXPECT_EQ(matched_regions.size(), 2u);
}

TEST(GuardedValueFlowAdapterShape,
     KeepsCommonReturnSSAOnlyAndBuildsPerReturnPseudoInterfaces) {
  const char *IR = R"(
    define i32* @test(i1 %cond, i32** %p, i32* %a, i32* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32* %a, i32** %p
      ret i32* %a
    else:
      store i32* %b, i32** %p
      ret i32* %b
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

  GuardedValueFlowReturnNode *common_return = nullptr;
  for (const auto &node_ptr : graph.nodes()) {
    if (node_ptr->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      common_return = dyn_cast<GuardedValueFlowReturnNode>(node_ptr.get());
      break;
    }
  }
  ASSERT_NE(common_return, nullptr);
  ASSERT_EQ(common_return->children().size(), 2u);
  for (const auto &edge : common_return->children())
    EXPECT_NE(edge.target->getKind(), GuardedValueFlowNode::Kind::LoadMemory);

  auto *pseudo_return = graph.getPseudoReturn(0);
  ASSERT_NE(pseudo_return, nullptr);
  EXPECT_EQ(pseudo_return->getIndex(), 0u);
  ASSERT_EQ(pseudo_return->children().size(), 2u);
  EXPECT_GE(pseudo_return->getAccessPath().getDepth(), 1);

  for (const auto &edge : pseudo_return->children()) {
    ASSERT_NE(edge.target, nullptr);
    EXPECT_EQ(edge.target->getKind(), GuardedValueFlowNode::Kind::LoadMemory);
    EXPECT_NE(pseudo_return->getReturnSite(edge.target), nullptr);
  }
}

TEST(GuardedValueFlowAdapterShape,
     PreservesIndexedPseudoArgumentsAndNestedAccessPaths) {
  const char *IR = R"(
    define void @deep(i32**** %root, i32* %value) {
    entry:
      %ppp = load i32***, i32**** %root
      %pp = load i32**, i32*** %ppp
      store i32* %value, i32** %pp
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("deep");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  bool saw_nested_path = false;
  for (unsigned idx = 0; idx < graph.pseudoArguments().size(); ++idx) {
    auto *pseudo_arg = graph.getPseudoArgument(idx);
    if (!pseudo_arg)
      continue;
    EXPECT_EQ(pseudo_arg->getIndex(), idx);
    EXPECT_GE(pseudo_arg->getAccessPath().getDepth(), 1);
    if (pseudo_arg->getAccessPath().getDepth() > 1)
      saw_nested_path = true;
  }

  EXPECT_TRUE(saw_nested_path);
}

TEST(GuardedValueFlowAdapterShape,
     DoesNotFabricateRecursivePseudoInterfacesWithoutBindings) {
  const char *IR = R"(
    define void @recur(i32** %p, i32* %v, i1 %cond) {
    entry:
      br i1 %cond, label %rec, label %exit
    rec:
      call void @recur(i32** %p, i32* %v, i1 false)
      ret void
    exit:
      store i32* %v, i32** %p
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseModule(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto pipeline = runPipeline(*M);
  Function *F = M->getFunction("recur");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  CallBase *recursive_call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      recursive_call = CB;
      break;
    }
  }
  ASSERT_NE(recursive_call, nullptr);

  auto *site = graph.findCallSite(recursive_call);
  ASSERT_NE(site, nullptr);
  EXPECT_EQ(site->getNumPseudoInputs(F), 0u);
  EXPECT_EQ(site->getNumPseudoOutputs(F), 0u);
}

} // namespace
