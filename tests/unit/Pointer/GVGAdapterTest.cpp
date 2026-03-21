#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#define private public
#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GSA/GSA.h"
#include "IR/GuardedValueFlow/GuardedValueFlowBuilder.h"
#include "IR/GuardedValueFlow/LotusAdapter.h"
#undef private

using namespace llvm;
using namespace llvm::gvg;

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
  ASSERT_EQ(pseudo_input->getAccessPath().getDepth(), 1);
  EXPECT_EQ(pseudo_input->getAccessPath().getOffset(0),
            load_input_it->second.getOffset());
  ASSERT_EQ(pseudo_input->children().size(), 1u);
  EXPECT_EQ(pseudo_input->children().front().target->getKind(),
            GuardedValueFlowNode::Kind::LoadMemory);

  ASSERT_GT(store_ptg->getOutputs().size(), 1u);
  auto *pseudo_output = store_site->getPseudoOutput(store_callee, 0);
  ASSERT_NE(pseudo_output, nullptr);
  EXPECT_EQ(pseudo_output->getIndex(), 0u);
  EXPECT_EQ(pseudo_output->getAccessPath().getBase(),
            store_ptg->getOutputs()[1]->getSymbolicInfo().getParentPtr());
  ASSERT_EQ(pseudo_output->getAccessPath().getDepth(), 1);
  EXPECT_EQ(pseudo_output->getAccessPath().getOffset(0),
            store_ptg->getOutputs()[1]->getSymbolicInfo().getOffset());
  EXPECT_TRUE(pseudo_output->children().empty());
  auto *pseudo_output_mem =
      graph.findStoreMemoryNode(pseudo_output->getLLVMValue(), store_call_inst);
  ASSERT_NE(pseudo_output_mem, nullptr);
  EXPECT_TRUE(containsChild(pseudo_output_mem, pseudo_output));
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
  EXPECT_EQ(load_mem->getMatchingRegions().size(), 2u);
  for (const auto &match : load_mem->getMatchingRegions()) {
    EXPECT_EQ(match.provenance.getKind(), ConditionRef::Kind::SemanticPathCond);
    EXPECT_NE(match.region, nullptr);
    EXPECT_TRUE(match.region->isInterfaceRegion());
    ASSERT_NE(match.region->getConditionNode(), nullptr);
    EXPECT_EQ(match.region->getConditionNode()->getRegion(), match.region);
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
  auto *first_mem = graph.findLoadMemoryNode(first);
  auto *second_mem = graph.findLoadMemoryNode(second);
  ASSERT_NE(first_value, nullptr);
  ASSERT_NE(second_value, nullptr);
  ASSERT_NE(first_mem, nullptr);
  ASSERT_NE(second_mem, nullptr);
  ASSERT_EQ(first_value->children().size(), 1u);
  ASSERT_EQ(second_value->children().size(), 1u);
  EXPECT_EQ(first_value->children().front().target, first_mem);
  EXPECT_EQ(second_value->children().front().target, second_mem);
  EXPECT_EQ(first_mem, second_mem);
  EXPECT_EQ(first_mem->getMatchingRegions().size(), second_mem->getMatchingRegions().size());
}

TEST(GVGAdapter, NonPointerLoadsAlsoReceiveMatchedStoreMemoryNodes) {
  const char *IR = R"(
    define i32 @test(i1 %cond, i32* %p) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32 7, i32* %p
      br label %merge
    else:
      store i32 9, i32* %p
      br label %merge
    merge:
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  LoadInst *load = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      load = LI;
  }
  ASSERT_NE(load, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *load_value = graph.findNode(load);
  auto *load_mem = graph.findLoadMemoryNode(load);
  ASSERT_NE(load_value, nullptr);
  ASSERT_NE(load_mem, nullptr);
  ASSERT_EQ(load_value->children().size(), 1u);
  EXPECT_EQ(load_value->children().front().target, load_mem);
  EXPECT_EQ(load_mem->children().size(), 2u);
  EXPECT_EQ(load_mem->getMatchingRegions().size(), 2u);
  for (const auto &match : load_mem->getMatchingRegions()) {
    EXPECT_EQ(match.provenance.getKind(), ConditionRef::Kind::SemanticPathCond);
    EXPECT_NE(match.region, nullptr);
    EXPECT_TRUE(match.region->isInterfaceRegion());
    ASSERT_NE(match.region->getConditionNode(), nullptr);
    EXPECT_EQ(match.region->getConditionNode()->getRegion(), match.region);
  }
}

TEST(GVGAdapter, RecordsPerCalleeCallTargetConditions) {
  const char *IR = R"(
    define void @left() {
    entry:
      ret void
    }

    define void @right() {
    entry:
      ret void
    }

    define void @test(i1 %cond) {
    entry:
      %slot = alloca void ()*
      %choice = select i1 %cond, void ()* @left, void ()* @right
      store void ()* %choice, void ()** %slot
      %fp = load void ()*, void ()** %slot
      call void %fp()
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I))
      call = CB;
  }
  ASSERT_NE(call, nullptr);

  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);

  auto *ptg = result.lotus->getPtGraph(F);
  ASSERT_NE(ptg, nullptr);
  auto resolved_it = ptg->getResolvedCallTargets().find(call);
  if (site->getCallees().empty() &&
      resolved_it == ptg->getResolvedCallTargets().end()) {
    GTEST_SKIP() << "LotusAA did not materialize indirect call targets for "
                    "this synthetic case";
  }

  Function *left = M->getFunction("left");
  Function *right = M->getFunction("right");
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(site->getCallees().size(), 2u);
  if (resolved_it == ptg->getResolvedCallTargets().end()) {
    EXPECT_FALSE(site->hasCalleeCondition(left));
    EXPECT_FALSE(site->hasCalleeCondition(right));
    EXPECT_EQ(site->getCalleeCondition(left).getKind(), ConditionRef::Kind::None);
    EXPECT_EQ(site->getCalleeCondition(right).getKind(), ConditionRef::Kind::None);
  } else {
    for (const auto &target : resolved_it->second) {
      if (target.second)
        EXPECT_TRUE(site->hasCalleeCondition(target.first));
      else
        EXPECT_FALSE(site->hasCalleeCondition(target.first));
      auto kind = site->getCalleeCondition(target.first).getKind();
      auto *region = site->getCalleeConditionRegion(target.first);
      if (target.second) {
        EXPECT_EQ(kind, ConditionRef::Kind::SemanticPathCond);
        ASSERT_NE(region, nullptr);
        EXPECT_TRUE(region->isInterfaceRegion());
        ASSERT_NE(region->getConditionNode(), nullptr);
        EXPECT_EQ(region->getConditionNode()->getRegion(), region);
        EXPECT_EQ(region->getInterfacePathCondition(), target.second);
      } else {
        EXPECT_EQ(kind, ConditionRef::Kind::None);
        EXPECT_EQ(region, nullptr);
      }
    }
  }
}

TEST(GVGAdapter, MaterializesSummaryNodesAndPseudoOutputIndices) {
  int old_ap_level = IntraLotusAAConfig::lotus_restrict_ap_level;
  int old_inline_size = IntraLotusAAConfig::lotus_restrict_inline_size;
  IntraLotusAAConfig::lotus_restrict_ap_level = 0;
  IntraLotusAAConfig::lotus_restrict_inline_size = -1;

  const char *IR = R"(
    define void @callee(i32*** %p, i32* %v) {
    entry:
      %slot = load i32**, i32*** %p
      store i32* %v, i32** %slot
      ret void
    }

    define void @test(i32*** %p, i32* %v) {
    entry:
      call void @callee(i32*** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipeline(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *F = M->getFunction("test");
  Function *Callee = M->getFunction("callee");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(Callee, nullptr);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *CB = dyn_cast<CallBase>(&I))
      call = CB;
  }
  ASSERT_NE(call, nullptr);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);

  auto *callee_ptg = result.lotus->getPtGraph(Callee);
  ASSERT_NE(callee_ptg, nullptr);

  bool has_any_summary_input = false;
  for (const auto *bucket : callee_ptg->getSummaryInputs()) {
    if (bucket && !bucket->empty()) {
      has_any_summary_input = true;
      break;
    }
  }
  bool has_any_summary_output = false;
  for (const auto *bucket : callee_ptg->getSummaryOutputs()) {
    if (bucket && !bucket->empty()) {
      has_any_summary_output = true;
      break;
    }
  }
  if (!has_any_summary_input && !has_any_summary_output)
    GTEST_SKIP() << "LotusAA did not materialize summary buckets for this synthetic case";

  bool saw_input_summary = false;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size(); ++bucket) {
    const auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
    if (!summary_inputs || summary_inputs->empty())
      continue;
    saw_input_summary = true;
    auto *node = site->getInputSummaryNode(Callee, bucket);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->getKind(), GuardedValueFlowNode::Kind::CallSiteArgumentSummary);
    auto *summary = dyn_cast<GuardedValueFlowCallSummaryNode>(node);
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->getSummaryIndex(), bucket);
    ASSERT_EQ(summary->children().size(), 1u);
    EXPECT_EQ(summary->children().front().target->getKind(),
              GuardedValueFlowNode::Kind::LoadMemory);
  }

  bool saw_output_summary = false;
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryOutputs().size(); ++bucket) {
    const auto *summary_outputs = callee_ptg->getSummaryOutputs()[bucket];
    if (!summary_outputs || summary_outputs->empty())
      continue;
    saw_output_summary = true;
    auto *node = site->getOutputSummaryNode(Callee, bucket);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->getKind(), GuardedValueFlowNode::Kind::CallSiteReturnSummary);
    auto *summary = dyn_cast<GuardedValueFlowCallSummaryNode>(node);
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->getSummaryIndex(), bucket);
    ASSERT_EQ(summary->children().size(), 1u);
    EXPECT_EQ(summary->children().front().target->getKind(),
              GuardedValueFlowNode::Kind::LoadMemory);
  }

  EXPECT_EQ(saw_input_summary, has_any_summary_input);
  EXPECT_EQ(saw_output_summary, has_any_summary_output);
}

TEST(GVGAdapter, DropsSummaryInputNodesWhenBindingsAreIncomplete) {
  int old_ap_level = IntraLotusAAConfig::lotus_restrict_ap_level;
  int old_inline_size = IntraLotusAAConfig::lotus_restrict_inline_size;
  IntraLotusAAConfig::lotus_restrict_ap_level = 0;
  IntraLotusAAConfig::lotus_restrict_inline_size = -1;

  const char *IR = R"(
    define void @callee(i32*** %p, i32* %v) {
    entry:
      %slot = load i32**, i32*** %p
      store i32* %v, i32** %slot
      ret void
    }

    define void @caller(i32*** %p, i32* %v) {
    entry:
      call void @callee(i32*** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  IntraLotusAAConfig::lotus_restrict_ap_level = old_ap_level;
  IntraLotusAAConfig::lotus_restrict_inline_size = old_inline_size;

  Function *caller = M->getFunction("caller");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));
  ASSERT_NE(result.lotus, nullptr);

  auto *caller_ptg = result.lotus->getPtGraph(caller);
  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(caller_ptg, nullptr);
  ASSERT_NE(callee_ptg, nullptr);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  Value *missing_binding = nullptr;
  for (const auto *bucket : callee_ptg->getSummaryInputs()) {
    if (bucket && !bucket->empty()) {
      missing_binding = *bucket->begin();
      break;
    }
  }
  if (!missing_binding)
    GTEST_SKIP() << "LotusAA did not materialize summary input buckets for this synthetic case";

  auto call_it = caller_ptg->func_arg.find(call);
  ASSERT_NE(call_it, caller_ptg->func_arg.end());
  auto callee_it = call_it->second.find(callee);
  ASSERT_NE(callee_it, call_it->second.end());
  callee_it->second.erase(missing_binding);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  LotusGuardedValueFlowAdapterPass adapter;
  adapter.adaptFunction(graph, *caller_ptg, *result.lotus, *result.builder);

  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);
  for (unsigned bucket = 0; bucket < callee_ptg->getSummaryInputs().size(); ++bucket) {
    const auto *summary_inputs = callee_ptg->getSummaryInputs()[bucket];
    if (!summary_inputs || summary_inputs->empty())
      continue;
    EXPECT_EQ(site->getInputSummaryNode(callee, bucket), nullptr);
  }
}

TEST(GVGAdapter, DropsPseudoInputInterfaceWhenBindingIsRemoved) {
  const char *IR = R"(
    define void @callee(i32** %p, i32** %q) {
    entry:
      %a = load i32*, i32** %p
      %b = load i32*, i32** %q
      ret void
    }

    define void @caller(i32** %p, i32** %q) {
    entry:
      call void @callee(i32** %p, i32** %q)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  Function *caller = M->getFunction("caller");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));
  ASSERT_NE(result.lotus, nullptr);

  auto *callee_ptg = result.lotus->getPtGraph(callee);
  auto *caller_ptg = result.lotus->getPtGraph(caller);
  ASSERT_NE(callee_ptg, nullptr);
  ASSERT_NE(caller_ptg, nullptr);
  ASSERT_EQ(callee_ptg->getInputs().size(), 2u);

  SmallVector<Value *, 2> pseudo_inputs;
  for (const auto &input_item : callee_ptg->getInputs())
    pseudo_inputs.push_back(input_item.first);
  ASSERT_EQ(pseudo_inputs.size(), 2u);
  ASSERT_EQ(callee_ptg->getPseudoInputIndex(pseudo_inputs[0]), 0);
  ASSERT_EQ(callee_ptg->getPseudoInputIndex(pseudo_inputs[1]), 1);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  auto call_it = caller_ptg->func_arg.find(call);
  ASSERT_NE(call_it, caller_ptg->func_arg.end());
  auto callee_it = call_it->second.find(callee);
  ASSERT_NE(callee_it, call_it->second.end());
  callee_it->second.erase(pseudo_inputs[0]);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  LotusGuardedValueFlowAdapterPass adapter;
  adapter.adaptFunction(graph, *caller_ptg, *result.lotus, *result.builder);

  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);
  EXPECT_EQ(site->getPseudoInput(callee, 0), nullptr);
  EXPECT_EQ(site->getPseudoInput(callee, 1), nullptr);
  EXPECT_EQ(site->getNumPseudoInputs(callee), 0u);
}

TEST(GVGAdapter, DropsPseudoOutputInterfaceWhenBindingIsRemoved) {
  const char *IR = R"(
    define void @callee(i32** %p, i32* %v) {
    entry:
      store i32* %v, i32** %p
      ret void
    }

    define void @caller(i32** %p, i32* %v) {
    entry:
      call void @callee(i32** %p, i32* %v)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  Function *caller = M->getFunction("caller");
  Function *callee = M->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*caller));
  ASSERT_NE(result.lotus, nullptr);

  auto *caller_ptg = result.lotus->getPtGraph(caller);
  auto *callee_ptg = result.lotus->getPtGraph(callee);
  ASSERT_NE(caller_ptg, nullptr);
  ASSERT_NE(callee_ptg, nullptr);
  ASSERT_GT(callee_ptg->getOutputs().size(), 1u);

  CallBase *call = nullptr;
  for (Instruction &I : instructions(*caller)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  auto call_it = caller_ptg->func_ret.find(call);
  ASSERT_NE(call_it, caller_ptg->func_ret.end());
  auto callee_it = call_it->second.find(callee);
  ASSERT_NE(callee_it, call_it->second.end());
  ASSERT_GT(callee_it->second.size(), 1u);
  callee_it->second[1] = nullptr;

  GuardedValueFlowGraph &graph = result.builder->getGraph(*caller);
  LotusGuardedValueFlowAdapterPass adapter;
  adapter.adaptFunction(graph, *caller_ptg, *result.lotus, *result.builder);

  auto *site = graph.findCallSite(call);
  ASSERT_NE(site, nullptr);
  EXPECT_EQ(site->getPseudoOutput(callee, 0), nullptr);
  EXPECT_EQ(site->getNumPseudoOutputs(callee), 0u);
}

TEST(GVGAdapter, KeepsPseudoArgumentsDistinctWhenInterfaceOverlapsFormal) {
  const char *IR = R"(
    @g = global i8 0

    define void @test(i32** %p) {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  auto result = runPipelineWithoutAdapter(*M);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(result.builder->hasGraphFor(*F));
  ASSERT_NE(result.lotus, nullptr);

  auto *ptg = result.lotus->getPtGraph(F);
  ASSERT_NE(ptg, nullptr);
  Argument *formal = F->arg_empty() ? nullptr : &*F->arg_begin();
  GlobalVariable *global = M->getNamedGlobal("g");
  ASSERT_NE(formal, nullptr);
  ASSERT_NE(global, nullptr);
  ptg->getInputs()[formal] = IntraLotusAA::AccessPath(global, 0);

  GuardedValueFlowGraph &graph = result.builder->getGraph(*F);
  auto *common_arg = graph.findNode(formal);
  ASSERT_NE(common_arg, nullptr);
  EXPECT_EQ(common_arg->getKind(), GuardedValueFlowNode::Kind::CommonArgument);

  LotusGuardedValueFlowAdapterPass adapter;
  adapter.adaptFunction(graph, *ptg, *result.lotus, *result.builder);

  GuardedValueFlowNode *pseudo_arg = nullptr;
  for (GuardedValueFlowNode *node : graph.pseudoArguments()) {
    if (node && node->getLLVMValue() == formal) {
      pseudo_arg = node;
      break;
    }
  }

  ASSERT_NE(pseudo_arg, nullptr);
  EXPECT_EQ(pseudo_arg->getKind(), GuardedValueFlowNode::Kind::PseudoArgument);
  EXPECT_NE(pseudo_arg, common_arg);
}

TEST(GVGAdapter, SafeLinkUsesEffectiveChildForMatchingAndRejectsInvalidTypes) {
  const char *IR = R"(
    define void @test() {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);
  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  GuardedValueFlowGraph graph(F);
  BasicBlock *entry = &F->getEntryBlock();
  auto *ret = cast<ReturnInst>(entry->getTerminator());

  auto *load_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt64Ty(Ctx), &graph,
      entry, nullptr, ret);
  auto *store_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::StoreMemory, Type::getInt32Ty(Ctx), &graph,
      entry, nullptr, ret);
  auto *cond = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt1Ty(Ctx), &graph,
      entry, nullptr, ret);
  auto *region =
      graph.findOrCreateUnitRegion(cond, true, entry, ConditionRef::none());
  ASSERT_NE(region, nullptr);

  auto *linked = LotusGuardedValueFlowAdapterPass::safeLink(
      graph, load_mem, store_mem, 0.75f, ConditionRef::none());
  ASSERT_NE(linked, nullptr);
  EXPECT_NE(linked, store_mem);
  EXPECT_EQ(linked->getKind(), GuardedValueFlowNode::Kind::CastOpcode);
  ASSERT_EQ(load_mem->children().size(), 1u);
  EXPECT_EQ(load_mem->children().front().target, linked);
  EXPECT_FLOAT_EQ(load_mem->children().front().confidence, 0.75f);
  ASSERT_EQ(linked->children().size(), 1u);
  EXPECT_EQ(linked->children().front().target, store_mem);
  EXPECT_TRUE(linked->containsParent(load_mem));
  EXPECT_TRUE(store_mem->containsParent(linked));
  EXPECT_FALSE(store_mem->containsParent(load_mem));

  load_mem->addMatchingRegion(linked, region, ConditionRef::none());
  EXPECT_EQ(load_mem->getMatchingRegion(linked), region);
  EXPECT_EQ(load_mem->getMatchingRegion(store_mem), nullptr);

  auto *aggregate_parent = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand,
      StructType::get(Ctx, {Type::getInt32Ty(Ctx), Type::getInt32Ty(Ctx)}),
      &graph,
      entry, nullptr, ret);
  auto *scalar_child = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt32Ty(Ctx), &graph,
      entry, nullptr, ret);
  EXPECT_EQ(LotusGuardedValueFlowAdapterPass::safeLink(graph, aggregate_parent,
                                                       scalar_child),
            nullptr);
  EXPECT_TRUE(aggregate_parent->children().empty());
}

} // namespace
