#include "IR/PDG/Analysis/PDGQuery.h"
#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>

using namespace llvm;
using namespace pdg;

namespace {

class PDGQueryTest : public ::testing::Test {
protected:
  void SetUp() override { graph.reset(); }

  void TearDown() override {
    loop_info.reset();
    post_dominator_tree.reset();
    dominator_tree.reset();
    module.reset();
    context.reset();
    graph.reset();
  }

  Node *addNode(GraphNodeType type = GraphNodeType::INST_OTHER) {
    Node *node = new Node(type);
    graph.addNode(*node);
    return node;
  }

  Node *addValueNode(Value &value,
                     GraphNodeType type = GraphNodeType::INST_OTHER) {
    Node *node = new Node(value, type);
    graph.addNode(*node);
    graph.getValueNodeMap()[&value] = node;
    return node;
  }

  Edge *addEdge(Node *src, Node *dst, EdgeType type) {
    EXPECT_NE(src, nullptr);
    EXPECT_NE(dst, nullptr);
    Edge *edge = new Edge(src, dst, type);
    src->addOutEdge(*edge);
    dst->addInEdge(*edge);
    graph.addEdge(*edge);
    return edge;
  }

  bool loadModule(const char *ir) {
    context = std::make_unique<LLVMContext>();
    SMDiagnostic error;
    module = parseIR(MemoryBuffer::getMemBuffer(ir)->getMemBufferRef(), error,
                     *context);
    return module != nullptr;
  }

  template <typename InstTy>
  InstTy *findInstruction(Function &function, StringRef name = "") {
    for (auto &block : function) {
      for (auto &inst : block) {
        InstTy *candidate = dyn_cast<InstTy>(&inst);
        if (candidate == nullptr)
          continue;
        if (name.empty() || inst.getName() == name)
          return candidate;
      }
    }
    return nullptr;
  }

  LLVMQueryContext buildLLVMQueryContext(Function &function) {
    dominator_tree = std::make_unique<DominatorTree>(function);
    post_dominator_tree = std::make_unique<PostDominatorTree>();
    post_dominator_tree->recalculate(function);
    loop_info = std::make_unique<LoopInfo>(*dominator_tree);

    LLVMQueryContext llvm_context;
    llvm_context.function = &function;
    llvm_context.dominator_tree = dominator_tree.get();
    llvm_context.post_dominator_tree = post_dominator_tree.get();
    llvm_context.loop_info = loop_info.get();
    llvm_context.memory_ssa = nullptr;
    return llvm_context;
  }

  std::string runCommand(const std::string &command) {
    std::array<char, 256> buffer;
    std::string output;
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
      return output;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
      output += buffer.data();
    pclose(pipe);
    return output;
  }

  std::string benchmarkPath() {
    std::vector<std::string> search_paths;
    search_paths.push_back("../benchmarks/spec2006/998.specrand.bc");
    search_paths.push_back("benchmarks/spec2006/998.specrand.bc");

    for (size_t i = 0; i < search_paths.size(); ++i) {
      struct stat st;
      if (stat(search_paths[i].c_str(), &st) == 0)
        return search_paths[i];
    }
    return "";
  }

  ProgramGraph &graph = ProgramGraph::getInstance();
  std::unique_ptr<LLVMContext> context;
  std::unique_ptr<Module> module;
  std::unique_ptr<DominatorTree> dominator_tree;
  std::unique_ptr<PostDominatorTree> post_dominator_tree;
  std::unique_ptr<LoopInfo> loop_info;
};

TEST_F(PDGQueryTest, SliceAndDependenceQueriesTrackExactPredecessors) {
  Node *a = addNode();
  Node *b = addNode();
  Node *c = addNode();
  Node *pred = addNode(GraphNodeType::INST_BR);

  addEdge(a, b, EdgeType::DATA_DEF_USE);
  addEdge(b, c, EdgeType::DATA_RAW);
  addEdge(pred, c, EdgeType::CONTROLDEP_BR);

  PDGCriteria criteria;
  criteria.nodes.insert(a);

  PDGQueryOptions all_options;
  all_options.edge_preset = PDGEdgePreset::All;

  SliceQuery slice_query(graph);
  PDGQueryResult forward = slice_query.forward(criteria, all_options);
  EXPECT_EQ(forward.nodes.size(), 3u);
  EXPECT_TRUE(forward.nodes.count(a));
  EXPECT_TRUE(forward.nodes.count(b));
  EXPECT_TRUE(forward.nodes.count(c));
  ASSERT_EQ(forward.predecessors[b].size(), 1u);
  EXPECT_TRUE(forward.predecessors[b].count(a));
  ASSERT_EQ(forward.predecessors[c].size(), 1u);
  EXPECT_TRUE(forward.predecessors[c].count(b));

  PDGCriteria sink;
  sink.nodes.insert(c);
  PDGQueryOptions data_options;
  data_options.edge_preset = PDGEdgePreset::Data;

  PDGQueryResult backward = slice_query.backward(sink, data_options);
  EXPECT_EQ(backward.nodes.size(), 3u);
  EXPECT_TRUE(backward.nodes.count(a));
  EXPECT_TRUE(backward.nodes.count(b));
  EXPECT_TRUE(backward.nodes.count(c));
  EXPECT_FALSE(backward.nodes.count(pred));

  DependenceQuery dependence_query(graph);
  PDGQueryResult shortest =
      dependence_query.shortestPath(criteria, sink, all_options);
  ASSERT_EQ(shortest.witness_paths.size(), 1u);
  ASSERT_EQ(shortest.witness_paths[0].nodes.size(), 3u);
  EXPECT_EQ(shortest.witness_paths[0].nodes[0], a);
  EXPECT_EQ(shortest.witness_paths[0].nodes[1], b);
  EXPECT_EQ(shortest.witness_paths[0].nodes[2], c);
  EXPECT_EQ(dependence_query.distance(criteria, sink, all_options), 2u);

  DataFlowQuery dataflow_query(graph);
  PDGQueryResult reaching_defs =
      dataflow_query.reachingDefinitions(sink, data_options);
  EXPECT_EQ(reaching_defs.nodes, backward.nodes);
}

TEST_F(PDGQueryTest, ContextSensitiveSliceRejectsMismatchedCaller) {
  Node *call_a = addNode(GraphNodeType::INST_FUNCALL);
  Node *call_b = addNode(GraphNodeType::INST_FUNCALL);
  Node *entry = addNode(GraphNodeType::FUNC_ENTRY);
  Node *body = addNode();
  Node *ret = addNode(GraphNodeType::INST_RET);

  addEdge(call_a, entry, EdgeType::CONTROLDEP_CALLINV);
  addEdge(call_b, entry, EdgeType::CONTROLDEP_CALLINV);
  addEdge(entry, body, EdgeType::DATA_DEF_USE);
  addEdge(body, ret, EdgeType::DATA_DEF_USE);
  addEdge(ret, call_a, EdgeType::CONTROLDEP_CALLRET);
  addEdge(ret, call_b, EdgeType::CONTROLDEP_CALLRET);

  PDGCriteria criteria;
  criteria.nodes.insert(call_a);

  PDGQueryOptions options;
  options.edge_preset = PDGEdgePreset::TransformLegality;
  options.context_mode = PDGContextMode::ContextSensitive;

  SliceQuery slice_query(graph);
  PDGQueryResult result = slice_query.forward(criteria, options);
  EXPECT_EQ(result.nodes.size(), 4u);
  EXPECT_TRUE(result.nodes.count(call_a));
  EXPECT_TRUE(result.nodes.count(entry));
  EXPECT_TRUE(result.nodes.count(body));
  EXPECT_TRUE(result.nodes.count(ret));
  EXPECT_FALSE(result.nodes.count(call_b));
}

TEST_F(PDGQueryTest, ThinSliceExcludesPointerFlowAndIsStrictSubset) {
  constexpr const char *IR = R"(
    define void @f(i32* %p, i32 %v) {
    entry:
      %ptr = getelementptr i32, i32* %p, i64 0
      store i32 %v, i32* %ptr
      %load = load i32, i32* %ptr
      ret void
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  Function *function = module->getFunction("f");
  ASSERT_NE(function, nullptr);

  Function::arg_iterator arg_it = function->arg_begin();
  Argument &ptr_arg = *arg_it++;
  Argument &val_arg = *arg_it;
  GetElementPtrInst *gep = findInstruction<GetElementPtrInst>(*function, "ptr");
  StoreInst *store = findInstruction<StoreInst>(*function);
  LoadInst *load = findInstruction<LoadInst>(*function, "load");
  ASSERT_NE(gep, nullptr);
  ASSERT_NE(store, nullptr);
  ASSERT_NE(load, nullptr);

  Node *ptr_node = addValueNode(ptr_arg, GraphNodeType::VAR_OTHER);
  Node *val_node = addValueNode(val_arg, GraphNodeType::VAR_OTHER);
  Node *gep_node = addValueNode(*gep);
  Node *store_node = addValueNode(*store);
  Node *load_node = addValueNode(*load);

  addEdge(ptr_node, gep_node, EdgeType::DATA_DEF_USE);
  addEdge(gep_node, store_node, EdgeType::DATA_DEF_USE);
  addEdge(val_node, store_node, EdgeType::DATA_DEF_USE);
  addEdge(store_node, load_node, EdgeType::DATA_ALIAS);
  addEdge(gep_node, load_node, EdgeType::DATA_DEF_USE);

  PDGCriteria criteria;
  criteria.nodes.insert(load_node);

  PDGQueryOptions full_options;
  full_options.edge_preset = PDGEdgePreset::Data;

  PDGQueryOptions thin_options = full_options;
  thin_options.slice_flavor = SliceFlavor::Thin;

  SliceQuery slice_query(graph);
  PDGQueryResult full = slice_query.backward(criteria, full_options);
  PDGQueryResult thin = slice_query.backward(criteria, thin_options);

  EXPECT_TRUE(full.nodes.count(ptr_node));
  EXPECT_TRUE(full.nodes.count(gep_node));
  EXPECT_EQ(thin.nodes.size(), 3u);
  EXPECT_TRUE(thin.nodes.count(load_node));
  EXPECT_TRUE(thin.nodes.count(store_node));
  EXPECT_TRUE(thin.nodes.count(val_node));
  EXPECT_FALSE(thin.nodes.count(gep_node));
  EXPECT_FALSE(thin.nodes.count(ptr_node));
}

TEST_F(PDGQueryTest, TransformAndSchedulingQueriesUseLLVMContext) {
  constexpr const char *IR = R"(
    define i32 @g(i32 %x) {
    entry:
      %a = add i32 %x, 1
      %b = mul i32 %a, 2
      ret i32 %b
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  Function *function = module->getFunction("g");
  ASSERT_NE(function, nullptr);
  BinaryOperator *a_inst = findInstruction<BinaryOperator>(*function, "a");
  BinaryOperator *b_inst = findInstruction<BinaryOperator>(*function, "b");
  ASSERT_NE(a_inst, nullptr);
  ASSERT_NE(b_inst, nullptr);

  Node *a_node = addValueNode(*a_inst);
  Node *b_node = addValueNode(*b_inst);
  addEdge(a_node, b_node, EdgeType::DATA_DEF_USE);

  LLVMQueryContext llvm_context = buildLLVMQueryContext(*function);
  TransformQuery transform_query(graph);
  MotionCheckResult motion =
      transform_query.canMoveEarlier(*b_node, *a_node, llvm_context);
  EXPECT_FALSE(motion.legal);
  EXPECT_FALSE(motion.blocking_path.empty());

  Node *c = addNode();
  addEdge(a_node, c, EdgeType::DATA_DEF_USE);
  std::set<Node *> region;
  region.insert(a_node);
  region.insert(b_node);
  region.insert(c);

  PDGQueryResult ready = transform_query.readySet(
      PDGQueryScope::nodeSet(region), std::set<Node *>(), llvm_context);
  EXPECT_EQ(ready.nodes.size(), 1u);
  EXPECT_TRUE(ready.nodes.count(a_node));

  std::vector<std::set<Node *>> levels =
      transform_query.topologicalLevels(PDGQueryScope::nodeSet(region),
                                       llvm_context);
  ASSERT_EQ(levels.size(), 2u);
  EXPECT_TRUE(levels[0].count(a_node));
  EXPECT_TRUE(levels[1].count(b_node));
  EXPECT_TRUE(levels[1].count(c));
  EXPECT_EQ(transform_query.criticalPathLength(PDGQueryScope::nodeSet(region),
                                               llvm_context),
            1u);
}

TEST_F(PDGQueryTest, TransformQueryRejectsMemoryMotionWithoutMemorySSA) {
  constexpr const char *IR = R"(
    define i32 @h(i32* %p, i32 %v) {
    entry:
      store i32 %v, i32* %p
      %x = load i32, i32* %p
      ret i32 %x
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  Function *function = module->getFunction("h");
  ASSERT_NE(function, nullptr);
  StoreInst *store = findInstruction<StoreInst>(*function);
  LoadInst *load = findInstruction<LoadInst>(*function, "x");
  ASSERT_NE(store, nullptr);
  ASSERT_NE(load, nullptr);

  Node *store_node = addValueNode(*store);
  Node *load_node = addValueNode(*load);
  LLVMQueryContext llvm_context = buildLLVMQueryContext(*function);

  TransformQuery transform_query(graph);
  MotionCheckResult motion =
      transform_query.canMoveEarlier(*store_node, *load_node, llvm_context);
  EXPECT_FALSE(motion.legal);
  EXPECT_NE(motion.reason.find("MemorySSA"), std::string::npos);
}

TEST_F(PDGQueryTest, DiffQueryReportsFunctionImpactSummary) {
  constexpr const char *IR = R"(
    define i32 @f(i32 %x) {
    entry:
      %a = add i32 %x, 1
      %b = mul i32 %a, 2
      %c = sub i32 %b, 3
      ret i32 %c
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  Function *function = module->getFunction("f");
  ASSERT_NE(function, nullptr);
  BinaryOperator *a_inst = findInstruction<BinaryOperator>(*function, "a");
  BinaryOperator *b_inst = findInstruction<BinaryOperator>(*function, "b");
  BinaryOperator *c_inst = findInstruction<BinaryOperator>(*function, "c");
  ASSERT_NE(a_inst, nullptr);
  ASSERT_NE(b_inst, nullptr);
  ASSERT_NE(c_inst, nullptr);

  Node *a_node = addValueNode(*a_inst);
  Node *b_node = addValueNode(*b_inst);
  Node *c_node = addValueNode(*c_inst);
  addEdge(a_node, b_node, EdgeType::DATA_DEF_USE);
  addEdge(b_node, c_node, EdgeType::DATA_DEF_USE);

  PDGQueryResult before;
  before.nodes.insert(a_node);
  before.nodes.insert(b_node);
  PDGQueryResult after;
  after.nodes.insert(a_node);
  after.nodes.insert(c_node);

  DiffQuery diff_query(graph);
  PDGQueryOptions options;
  options.edge_preset = PDGEdgePreset::Data;
  DiffQueryResult diff = diff_query.diff(before, after, options);

  EXPECT_FALSE(diff.isIdentical());
  EXPECT_EQ(diff.impact_summary.functions["f"], 2u);
}

TEST_F(PDGQueryTest, PropertyCriteriaResolverFindsReachErrorCall) {
  constexpr const char *IR = R"(
    declare void @reach_error()
    define void @main() {
    entry:
      call void @reach_error()
      ret void
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  Function *function = module->getFunction("main");
  ASSERT_NE(function, nullptr);
  CallInst *call = findInstruction<CallInst>(*function);
  ASSERT_NE(call, nullptr);
  Node *call_node = addValueNode(*call, GraphNodeType::INST_FUNCALL);

  PropertySpec spec;
  std::string error;
  ASSERT_TRUE(PropertySpec::parseFromString(
      "CHECK( init(main()), LTL(G ! call(reach_error())) )\n", spec, error))
      << error;

  PDGCriteria criteria;
  criteria.property_specs.push_back(spec);
  PDGCriteriaResolver resolver(graph);
  PDGQueryResult resolved = resolver.resolve(criteria, PDGQueryOptions(), module.get());
  EXPECT_EQ(resolved.nodes.size(), 1u);
  EXPECT_TRUE(resolved.nodes.count(call_node));
}

TEST_F(PDGQueryTest, SummaryQueryProducesExpectedBucketsAndCaches) {
  constexpr const char *IR = R"(
    @G = global i32 0
    declare i8* @malloc(i64)
    define i32 @f(i32 %x) {
    entry:
      %a = add i32 %x, 1
      store i32 %a, i32* @G
      %p = call i8* @malloc(i64 4)
      br label %exit
    exit:
      ret i32 %a
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  Function *function = module->getFunction("f");
  GlobalVariable *global = module->getGlobalVariable("G");
  ASSERT_NE(function, nullptr);
  ASSERT_NE(global, nullptr);
  BinaryOperator *add_inst = findInstruction<BinaryOperator>(*function, "a");
  CallInst *malloc_call = nullptr;
  StoreInst *store_inst = nullptr;
  BranchInst *branch_inst = nullptr;
  ReturnInst *ret_inst = nullptr;
  for (auto &bb : *function) {
    for (auto &inst : bb) {
      if (!malloc_call)
        malloc_call = dyn_cast<CallInst>(&inst);
      if (!store_inst)
        store_inst = dyn_cast<StoreInst>(&inst);
      if (!branch_inst)
        branch_inst = dyn_cast<BranchInst>(&inst);
      if (!ret_inst)
        ret_inst = dyn_cast<ReturnInst>(&inst);
    }
  }
  ASSERT_NE(add_inst, nullptr);
  ASSERT_NE(malloc_call, nullptr);
  ASSERT_NE(store_inst, nullptr);
  ASSERT_NE(branch_inst, nullptr);
  ASSERT_NE(ret_inst, nullptr);

  Function::arg_iterator arg_it = function->arg_begin();
  Argument &x_arg = *arg_it;

  Node *arg_node = addValueNode(x_arg, GraphNodeType::VAR_OTHER);
  Node *add_node = addValueNode(*add_inst);
  Node *global_node = addValueNode(*global, GraphNodeType::VAR_STATICALLOCMODULESCOPE);
  Node *malloc_node = addValueNode(*malloc_call, GraphNodeType::INST_FUNCALL);
  Node *branch_node = addValueNode(*branch_inst, GraphNodeType::INST_BR);
  Node *ret_node = addValueNode(*ret_inst, GraphNodeType::INST_RET);
  addValueNode(*store_inst);

  addEdge(arg_node, add_node, EdgeType::DATA_DEF_USE);
  addEdge(add_node, global_node, EdgeType::DATA_DEF_USE);
  addEdge(add_node, malloc_node, EdgeType::DATA_DEF_USE);
  addEdge(add_node, ret_node, EdgeType::DATA_DEF_USE);
  addEdge(branch_node, malloc_node, EdgeType::CONTROLDEP_BR);

  PDGCriteria criteria;
  criteria.function_names.push_back("f");
  SummaryQuery query(graph);
  SummaryQueryResult result =
      query.summarize(criteria, SummaryPolicy(), PDGQueryOptions(), module.get());

  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result.summary.function, function);
  EXPECT_EQ(result.summary.input_to_return.size(), 1u);
  EXPECT_EQ(result.summary.input_to_global_write.size(), 1u);
  EXPECT_EQ(result.summary.input_to_callsite.size(), 1u);
  EXPECT_EQ(result.summary.control_predicates.size(), 1u);
  EXPECT_EQ(result.summary.reachable_calls.size(), 1u);
  EXPECT_TRUE(result.summary.may_allocate_resource_kinds.count(ResourceKind::Heap));

  SummaryQueryResult cached =
      query.summarize(criteria, SummaryPolicy(), PDGQueryOptions(), module.get());
  EXPECT_GE(cached.diagnostics.summary_cache_hits, 1u);
}

TEST_F(PDGQueryTest, ImpactQueryDistinguishesDirectTransitiveAndChangedOnly) {
  constexpr const char *IR = R"(
    define i32 @callee(i32 %v) {
    entry:
      %x = add i32 %v, 1
      ret i32 %x
    }
    define i32 @main(i32 %a) {
    entry:
      %b = add i32 %a, 2
      %c = mul i32 %b, 3
      %d = call i32 @callee(i32 %c)
      ret i32 %d
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  Function *main_fn = module->getFunction("main");
  Function *callee_fn = module->getFunction("callee");
  ASSERT_NE(main_fn, nullptr);
  ASSERT_NE(callee_fn, nullptr);

  Argument &a_arg = *main_fn->arg_begin();
  BinaryOperator *b_inst = findInstruction<BinaryOperator>(*main_fn, "b");
  BinaryOperator *c_inst = findInstruction<BinaryOperator>(*main_fn, "c");
  CallInst *call_inst = findInstruction<CallInst>(*main_fn, "d");
  ReturnInst *main_ret = findInstruction<ReturnInst>(*main_fn);
  BinaryOperator *callee_add = findInstruction<BinaryOperator>(*callee_fn, "x");
  ReturnInst *callee_ret = findInstruction<ReturnInst>(*callee_fn);
  ASSERT_NE(b_inst, nullptr);
  ASSERT_NE(c_inst, nullptr);
  ASSERT_NE(call_inst, nullptr);
  ASSERT_NE(main_ret, nullptr);
  ASSERT_NE(callee_add, nullptr);
  ASSERT_NE(callee_ret, nullptr);

  Node *a_node = addValueNode(a_arg, GraphNodeType::VAR_OTHER);
  Node *b_node = addValueNode(*b_inst);
  Node *c_node = addValueNode(*c_inst);
  Node *call_node = addValueNode(*call_inst, GraphNodeType::INST_FUNCALL);
  Node *main_ret_node = addValueNode(*main_ret, GraphNodeType::INST_RET);
  Node *callee_entry = addValueNode(*callee_fn, GraphNodeType::FUNC_ENTRY);
  Node *callee_add_node = addValueNode(*callee_add);
  Node *callee_ret_node = addValueNode(*callee_ret, GraphNodeType::INST_RET);

  addEdge(a_node, b_node, EdgeType::DATA_DEF_USE);
  addEdge(b_node, c_node, EdgeType::DATA_DEF_USE);
  addEdge(c_node, call_node, EdgeType::DATA_DEF_USE);
  addEdge(call_node, main_ret_node, EdgeType::DATA_RET);
  addEdge(call_node, callee_entry, EdgeType::CONTROLDEP_CALLINV);
  addEdge(callee_entry, callee_add_node, EdgeType::DATA_DEF_USE);
  addEdge(callee_add_node, callee_ret_node, EdgeType::DATA_DEF_USE);
  addEdge(callee_ret_node, call_node, EdgeType::CONTROLDEP_CALLRET);

  PDGCriteria criteria;
  criteria.nodes.insert(a_node);
  PDGCriteria baseline;
  baseline.nodes.insert(b_node);
  PDGQueryOptions options;
  options.edge_preset = PDGEdgePreset::All;

  ImpactQuery query(graph);
  ImpactQueryResult result = query.analyze(criteria, ImpactPolicy(), options, module.get());
  EXPECT_EQ(result.directly_impacted_nodes.nodes.size(), 2u);
  EXPECT_TRUE(result.directly_impacted_nodes.nodes.count(b_node));
  EXPECT_TRUE(result.transitively_impacted_nodes.nodes.count(main_ret_node));
  EXPECT_TRUE(result.impacted_functions.count("main"));
  EXPECT_TRUE(result.impacted_functions.count("callee"));
  EXPECT_FALSE(result.ranked_impacts.empty());

  ImpactPolicy changed_only_policy;
  changed_only_policy.changed_only = true;
  ImpactQueryResult changed = query.analyzeAgainstBaseline(
      criteria, baseline, changed_only_policy, options, module.get());
  EXPECT_FALSE(changed.changed_only_diff.isIdentical());
  EXPECT_FALSE(changed.ranked_impacts.empty());
}

TEST_F(PDGQueryTest, ResourceFlowQueryUsesSummaryReuseAcrossFunctions) {
  constexpr const char *IR = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    define i8* @alloc_wrap() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret i8* %p
    }
    define void @caller() {
    entry:
      %q = call i8* @alloc_wrap()
      call void @free(i8* %q)
      ret void
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  Function *alloc_wrap = module->getFunction("alloc_wrap");
  Function *caller = module->getFunction("caller");
  ASSERT_NE(alloc_wrap, nullptr);
  ASSERT_NE(caller, nullptr);

  CallInst *malloc_call = findInstruction<CallInst>(*alloc_wrap, "p");
  ReturnInst *alloc_ret = findInstruction<ReturnInst>(*alloc_wrap);
  CallInst *alloc_wrap_call = nullptr;
  CallInst *free_call = nullptr;
  for (auto &bb : *caller) {
    for (auto &inst : bb) {
      CallInst *call = dyn_cast<CallInst>(&inst);
      if (call == nullptr)
        continue;
      if (call->getCalledFunction() &&
          call->getCalledFunction()->getName() == "alloc_wrap")
        alloc_wrap_call = call;
      else if (call->getCalledFunction() &&
               call->getCalledFunction()->getName() == "free")
        free_call = call;
    }
  }
  ASSERT_NE(malloc_call, nullptr);
  ASSERT_NE(alloc_ret, nullptr);
  ASSERT_NE(alloc_wrap_call, nullptr);
  ASSERT_NE(free_call, nullptr);

  Node *malloc_node = addValueNode(*malloc_call, GraphNodeType::INST_FUNCALL);
  Node *alloc_ret_node = addValueNode(*alloc_ret, GraphNodeType::INST_RET);
  Node *caller_entry = addValueNode(*caller, GraphNodeType::FUNC_ENTRY);
  Node *alloc_wrap_call_node =
      addValueNode(*alloc_wrap_call, GraphNodeType::INST_FUNCALL);
  Node *free_node = addValueNode(*free_call, GraphNodeType::INST_FUNCALL);

  addEdge(malloc_node, alloc_ret_node, EdgeType::DATA_RET);
  addEdge(alloc_wrap_call_node, caller_entry, EdgeType::CONTROLDEP_CALLINV);
  addEdge(alloc_ret_node, alloc_wrap_call_node, EdgeType::DATA_RET);
  addEdge(alloc_wrap_call_node, free_node, EdgeType::DATA_DEF_USE);

  ResourceFlowQuery query(graph);
  ResourceFlowQueryResult interproc = query.analyze(
      PDGCriteria(), ResourcePolicy(), PDGQueryOptions(), module.get());
  EXPECT_EQ(interproc.acquire_sites.size(), 1u);
  EXPECT_EQ(interproc.release_sites.size(), 1u);
  EXPECT_FALSE(interproc.resource_paths.empty());
  EXPECT_TRUE(interproc.resource_paths[0].released);
}

TEST_F(PDGQueryTest, ResourceFlowQueryFindsLeakAndDoubleRelease) {
  ASSERT_TRUE(loadModule(R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)
    define void @g() {
    entry:
      %p = call i8* @malloc(i64 4)
      call void @free(i8* %p)
      call void @free(i8* %p)
      ret void
    }
    define void @h() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret void
    }
  )"));
  Function *g = module->getFunction("g");
  Function *h = module->getFunction("h");
  ASSERT_NE(g, nullptr);
  ASSERT_NE(h, nullptr);
  std::vector<CallInst *> g_calls;
  for (auto &bb : *g)
    for (auto &inst : bb)
      if (CallInst *call = dyn_cast<CallInst>(&inst))
        g_calls.push_back(call);
  std::vector<CallInst *> h_calls;
  for (auto &bb : *h)
    for (auto &inst : bb)
      if (CallInst *call = dyn_cast<CallInst>(&inst))
        h_calls.push_back(call);
  ASSERT_EQ(g_calls.size(), 3u);
  ASSERT_EQ(h_calls.size(), 1u);
  Node *g_malloc = addValueNode(*g_calls[0], GraphNodeType::INST_FUNCALL);
  Node *g_free1 = addValueNode(*g_calls[1], GraphNodeType::INST_FUNCALL);
  Node *g_free2 = addValueNode(*g_calls[2], GraphNodeType::INST_FUNCALL);
  Node *h_malloc = addValueNode(*h_calls[0], GraphNodeType::INST_FUNCALL);
  addEdge(g_malloc, g_free1, EdgeType::DATA_DEF_USE);
  addEdge(g_malloc, g_free2, EdgeType::DATA_DEF_USE);

  ResourceFlowQuery query(graph);
  ResourceFlowQueryResult local = query.analyze(
      PDGCriteria(), ResourcePolicy(), PDGQueryOptions(), module.get());
  EXPECT_EQ(local.acquire_sites.size(), 2u);
  EXPECT_EQ(local.orphaned_resources.size(), 1u);
  EXPECT_EQ(local.double_release_candidates.size(), 2u);
}

TEST_F(PDGQueryTest, PdgQueryCliSmokeCoversNewAnalyses) {
  const std::string benchmark = benchmarkPath();
  if (benchmark.empty()) {
    GTEST_SKIP() << "benchmark bitcode not available";
    return;
  }

  const std::string binary =
      "/Users/rainoftime/Work/analysis/lotus/build/bin/pdg-query";
  {
    const std::string output = runCommand(
        binary + " " + benchmark +
        " --analysis summary --scope-function main --format json 2>&1");
    EXPECT_NE(output.find("\"function\""), std::string::npos);
  }
  {
    const std::string output = runCommand(
        binary + " " + benchmark +
        " --analysis impact --criteria-query \"MATCH (n:INST_RET) RETURN n\" "
        "--baseline-query \"MATCH (n:INST_BR) RETURN n\" --format json 2>&1");
    EXPECT_NE(output.find("\"ranked_impacts\""), std::string::npos);
  }
  {
    const std::string output = runCommand(
        binary + " " + benchmark +
        " --analysis resource-flow --format json 2>&1");
    EXPECT_NE(output.find("\"resource_kind_counts\""), std::string::npos);
  }
}

TEST_F(PDGQueryTest, BenchmarkBackedSmokeUsesNewSliceQueryWhenAvailable) {
  std::string path = benchmarkPath();
  if (path.empty()) {
    GTEST_SKIP() << "benchmark bitcode not available";
    return;
  }

  context = std::make_unique<LLVMContext>();
  SMDiagnostic error;
  module = parseIRFile(path, error, *context);
  ASSERT_TRUE(module != nullptr);

  auto &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  legacy::PassManager pm;
  pm.add(new DataDependencyGraph());
  pm.add(new ControlDependencyGraph());
  pm.add(new ProgramDependencyGraph());
  pm.run(*module);

  ProgramGraph::NodeSet::iterator begin = graph.begin();
  if (begin == graph.end()) {
    GTEST_SKIP() << "PDG build produced no nodes";
    return;
  }

  PDGCriteria criteria;
  criteria.nodes.insert(*begin);
  PDGQueryOptions options;
  options.edge_preset = PDGEdgePreset::All;

  SliceQuery slice_query(graph);
  PDGQueryResult result = slice_query.forward(criteria, options, module.get());
  EXPECT_FALSE(result.nodes.empty());
  EXPECT_TRUE(result.nodes.count(*begin));
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
