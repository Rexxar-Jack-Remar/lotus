#include <llvm/AsmParser/Parser.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GSA/GSA.h"
#include "IR/GuardedValueFlow/GuardedValueFlowBuilder.h"
#include "IR/GuardedValueFlow/LotusAdapter.h"

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

struct PipelineResult {
  std::unique_ptr<legacy::PassManager> pm;
  LotusAA *lotus{nullptr};
  GuardedValueFlowGraphBuilderPass *builder{nullptr};
};

std::unique_ptr<Module> parseAssembly(LLVMContext &Ctx, const char *IR) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("GVGAdapterTest", errs());
  return M;
}

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

TEST(GVGAdapter, MaterializesPseudoCallInterfaceNodes) {
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
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*F));

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);

  Instruction *load_call_inst = nullptr;
  Instruction *store_call_inst = nullptr;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Function *callee = CB->getCalledFunction();
      if (!callee)
        continue;
      if (callee->getName() == "load_arg")
        load_call_inst = &I;
      if (callee->getName() == "store_arg")
        store_call_inst = &I;
    }
  }

  auto *load_site = graph.findCallSite(load_call_inst);
  auto *store_site = graph.findCallSite(store_call_inst);
  ASSERT_NE(load_site, nullptr);
  ASSERT_NE(store_site, nullptr);

  Function *load_callee = M->getFunction("load_arg");
  Function *store_callee = M->getFunction("store_arg");
  ASSERT_NE(load_callee, nullptr);
  ASSERT_NE(store_callee, nullptr);

  auto *load_ptg = result.lotus->getPtGraph(load_callee);
  auto *store_ptg = result.lotus->getPtGraph(store_callee);
  ASSERT_NE(load_ptg, nullptr);
  ASSERT_NE(store_ptg, nullptr);

  EXPECT_EQ(load_site->getCallees().size(), 1u);
  EXPECT_EQ(store_site->getCallees().size(), 1u);
  EXPECT_EQ(load_site->getCallees().front(), load_callee);
  EXPECT_EQ(store_site->getCallees().front(), store_callee);
  EXPECT_EQ(load_site->getNumPseudoInputs(load_callee),
            static_cast<unsigned>(load_ptg->getInputs().size()));
  EXPECT_EQ(store_site->getNumPseudoOutputs(store_callee),
            store_ptg->getOutputs().empty()
                ? 0u
                : static_cast<unsigned>(store_ptg->getOutputs().size() - 1));
  EXPECT_NE(load_site->getCommonOutput(), nullptr);

  auto load_input_it = load_ptg->getInputs().begin();
  ASSERT_NE(load_input_it, load_ptg->getInputs().end());
  auto *pseudo_input = load_site->getPseudoInput(load_callee, 0);
  ASSERT_NE(pseudo_input, nullptr);
  EXPECT_EQ(pseudo_input->getIndex(), 0u);
  EXPECT_EQ(pseudo_input->getAccessPath().getBase(),
            load_input_it->second.getParentPtr());
  EXPECT_EQ(pseudo_input->getAccessPath().getOffset(),
            load_input_it->second.getOffset());
  ASSERT_EQ(pseudo_input->children().size(), 1u);
  EXPECT_EQ(pseudo_input->children().front().target->getKind(),
            GuardedValueFlowNode::Kind::LoadMemory);

  ASSERT_GT(store_ptg->getOutputs().size(), 1u);
  auto *pseudo_output = store_site->getPseudoOutput(store_callee, 0);
  ASSERT_NE(pseudo_output, nullptr);
  EXPECT_EQ(pseudo_output->getIndex(), 1u);
  EXPECT_EQ(pseudo_output->getAccessPath().getBase(),
            store_ptg->getOutputs()[1]->getSymbolicInfo().getParentPtr());
  EXPECT_EQ(pseudo_output->getAccessPath().getOffset(),
            store_ptg->getOutputs()[1]->getSymbolicInfo().getOffset());
  ASSERT_EQ(pseudo_output->children().size(), 1u);
  EXPECT_EQ(pseudo_output->children().front().target->getKind(),
            GuardedValueFlowNode::Kind::StoreMemory);
}

TEST(GVGAdapter, ConditionalLoadPreservesTwoMatchingConditions) {
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
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  LoadInst *load = nullptr;
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        load = LI;
        break;
      }
    }
  }
  ASSERT_NE(load, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_mem, nullptr);
  EXPECT_EQ(load_mem->children().size(), 2u);
  EXPECT_EQ(load_mem->getMatchingConditions().size(), 2u);
  for (const auto &match : load_mem->getMatchingConditions()) {
    EXPECT_EQ(match.second.getKind(), ConditionRef::Kind::SemanticPathCond);
  }
}

TEST(GVGAdapter, EquivalentLoadsReuseCanonicalLoadMemoryNode) {
  const char *IR = R"(
    define i32* @test(i32** %p) {
    entry:
      %v1 = load i32*, i32** %p
      %v2 = load i32*, i32** %p
      ret i32* %v2
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  LoadInst *first = nullptr;
  LoadInst *second = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!first)
        first = LI;
      else
        second = LI;
    }
  }
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *first_value = graph.findNode(first);
  auto *second_value = graph.findNode(second);
  ASSERT_NE(first_value, nullptr);
  ASSERT_NE(second_value, nullptr);
  ASSERT_EQ(first_value->children().size(), 1u);
  ASSERT_EQ(second_value->children().size(), 1u);
  EXPECT_EQ(first_value->children().front().target,
            second_value->children().front().target);
}

} // namespace
