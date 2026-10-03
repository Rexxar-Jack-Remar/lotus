#include "Dataflow/IFDS/Analyses/IDEExtendedTaintAnalysis.h"
#include "Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h"
#include "Dataflow/IFDS/Solver/IDESolver.h"
#include "Dataflow/IFDS/Solver/IFDSSolver.h"
#include "Dataflow/IFDS/Solver/PathAwareIFDSSolver.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>

namespace ifds {
namespace {

struct Bits {
  unsigned value = 0;
  bool operator==(Bits other) const { return value == other.value; }
};

template <typename Base> class ScalarFlow : public Base {
public:
  using Fact = const llvm::Value *;
  using FactSet = typename Base::FactSet;
  bool saw_unknown = false;
  Fact zero_fact() const override { return nullptr; }
  bool auto_add_zero() const override { return false; }
  FactSet initial_facts(const llvm::Function *function) override {
    return {function->arg_empty() ? nullptr : function->getArg(0)};
  }
  FactSet normal_flow(const llvm::Instruction *inst, const llvm::Instruction *,
                      const Fact &fact) override {
    FactSet result{fact};
    if (fact && !inst->getType()->isVoidTy())
      for (const auto &operand : inst->operands())
        if (operand.get() == fact)
          result.insert(inst);
    return result;
  }
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override {
    FactSet result;
    if (!fact)
      result.insert(nullptr);
    for (unsigned i = 0; i < call->arg_size() && i < callee->arg_size(); ++i)
      if (call->getArgOperand(i) == fact)
        result.insert(callee->getArg(i));
    return result;
  }
  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit,
                      const llvm::Instruction *, const llvm::Function *,
                      const Fact &fact, const Fact &) override {
    if (!fact)
      return {nullptr};
    auto *ret = llvm::dyn_cast<llvm::ReturnInst>(exit);
    if (ret && fact && ret->getReturnValue() == fact)
      return {call};
    return {};
  }
  FactSet call_to_return_flow(const llvm::CallBase *, const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const Fact &fact) override {
    for (const auto *callee : callees)
      saw_unknown |= callee == nullptr;
    return {fact};
  }
  const llvm::Value *sparse_fact_value(const Fact &fact) const override {
    return fact;
  }
  bool is_identity_flow(const llvm::Instruction *inst,
                        const llvm::Instruction *,
                        const Fact &fact) const override {
    if (!fact)
      return false;
    if (!inst->getType()->isVoidTy())
      for (const auto &operand : inst->operands())
        if (operand.get() == fact)
          return false;
    return true;
  }
};
using ScalarProblem = ScalarFlow<IFDSProblem<const llvm::Value *>>;

class ValueProblem : public ScalarFlow<IDEProblem<const llvm::Value *, Bits>> {
public:
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    return lift_ifds_initial_seeds(module, Bits{1});
  }
  Bits top_value() const override { return {7}; }
  Bits bottom_value() const override { return {0}; }
  Bits join(const Bits &a, const Bits &b) const override {
    return {a.value | b.value};
  }
  EdgeFunction normal_edge_function(const llvm::Instruction *inst,
                                    const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    if (inst->getName() == "one")
      return edge::constant<Bits>(Bits{1});
    if (inst->getName() == "two" || inst->getName() == "bump")
      return edge::constant<Bits>(Bits{2});
    return identity();
  }
  bool is_identity_edge(const llvm::Instruction *inst,
                        const llvm::Instruction *,
                        const Fact &) const override {
    return inst->getName() != "one" && inst->getName() != "two" &&
           inst->getName() != "bump";
  }
  EdgeFunction call_edge_function(const llvm::CallBase *,
                                  const llvm::Function *, const Fact &,
                                  const Fact &) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase *,
                                    const llvm::Function *,
                                    const llvm::Instruction *,
                                    const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    return identity();
  }
  EdgeFunction
  call_to_return_edge_function(const llvm::CallBase *,
                               const llvm::Instruction *,
                               llvm::ArrayRef<const llvm::Function *>,
                               const Fact &, const Fact &) override {
    return identity();
  }
};

const char *IndirectIR = R"(
  define i32 @identity(i32 %x) { ret i32 %x }
  define i32 @other(i32 %x) { ret i32 %x }
  define i32 @main(i32 %x) {
    %target = select i1 true, i32 (i32)* @identity, i32 (i32)* @other
    %result = call i32 %target(i32 %x)
    ret i32 %result
  }
)";

TEST(AnalysisSessionTest,
     InjectedProviderEntersAllIndirectBodiesAndKeepsUnknown) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, IndirectIR);
  auto provider = [&](const llvm::CallBase &) {
    return CalleeTargets{{module->getFunction("identity"),
                          module->getFunction("other"),
                          module->getFunction("identity")},
                         false};
  };
  auto *main = module->getFunction("main");
  auto *result = lotus::unittest::findInstructionByName(*main, "result");
  ScalarProblem problem;
  IFDSSolver<ScalarProblem> solver(problem);
  solver.set_callee_provider(provider);
  solver.solve(*module);
  EXPECT_EQ(
      solver.get_facts_at_entry(main->back().getTerminator()).count(result),
      1U);
  EXPECT_TRUE(problem.saw_unknown);
  for (auto *name : {"identity", "other"}) {
    auto *callee = module->getFunction(name);
    EXPECT_EQ(solver.get_facts_at_entry(&callee->front().front())
                  .count(callee->getArg(0)),
              1U);
  }
  ValueProblem ide_problem;
  IDESolver<ValueProblem> ide(ide_problem);
  ide.set_callee_provider(provider);
  ide.solve(*module);
  EXPECT_EQ(ide.get_value_at(main->back().getTerminator(), result).value, 1U);
  EXPECT_TRUE(ide_problem.saw_unknown);
}

TEST(AnalysisSessionTest, InjectedICFGAndPathAwareWrapperResolveIndirectCall) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, IndirectIR);
  auto graph = std::make_shared<dataflow::controlflow::LLVMInterCFG>(
      module.get(), [&](llvm::Instruction *) {
        return std::vector<llvm::Function *>{module->getFunction("identity")};
      });
  ScalarProblem problem;
  PathAwareIFDSSolver<ScalarProblem> solver(problem);
  solver.set_icfg(graph);
  solver.solve(*module);
  auto *main = module->getFunction("main");
  auto *result = lotus::unittest::findInstructionByName(*main, "result");
  EXPECT_EQ(
      solver.get_facts_at_entry(main->back().getTerminator()).count(result),
      1U);
  EXPECT_GT(solver.get_esg_edge_count(), 0U);
}

TEST(AnalysisSessionTest, CompleteProvidersDeduplicateTargetsWithoutUnknown) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, IndirectIR);
  ScalarProblem problem;
  IFDSSolver<ScalarProblem> solver(problem);
  solver.set_callee_provider([&](const llvm::CallBase &) {
    return CalleeTargets{
        {module->getFunction("identity"), module->getFunction("identity")},
        true};
  });
  solver.solve(*module);
  EXPECT_FALSE(problem.saw_unknown);
  std::vector<SummaryEdge<const llvm::Value *>> summaries;
  solver.get_summary_edges(summaries);
  EXPECT_EQ(summaries.size(), 1U);
}

TEST(AnalysisSessionTest, IndirectRecursiveSummariesDoNotCrossCallerFacts) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    define i32 @recursive(i32 %x) {
      br i1 true, label %base, label %recurse
    base:
      ret i32 %x
    recurse:
      %again = call i32 @recursive(i32 %x)
      ret i32 %again
    }
    define i32 @main(i32 %x) {
      %target = select i1 true, i32 (i32)* @recursive, i32 (i32)* @recursive
      %first = call i32 %target(i32 %x)
      %clean = call i32 %target(i32 0)
      ret i32 %first
    }
  )");
  auto provider = [&](const llvm::CallBase &) {
    return CalleeTargets{{module->getFunction("recursive")}, true};
  };
  auto *main = module->getFunction("main");
  auto *first = lotus::unittest::findInstructionByName(*main, "first");
  auto *clean = lotus::unittest::findInstructionByName(*main, "clean");
  ScalarProblem problem;
  IFDSSolver<ScalarProblem> solver(problem);
  solver.set_callee_provider(provider);
  solver.solve(*module);
  auto facts = solver.get_facts_at_entry(main->back().getTerminator());
  EXPECT_EQ(facts.count(first), 1U);
  EXPECT_EQ(facts.count(clean), 0U);
  ValueProblem ide_problem;
  IDESolver<ValueProblem> ide(ide_problem);
  ide.set_callee_provider(provider);
  ide.solve(*module);
  EXPECT_EQ(ide.get_value_at(main->back().getTerminator(), first).value, 1U);
  EXPECT_EQ(ide.get_value_at(main->back().getTerminator(), clean).value, 0U);
}

TEST(AnalysisSessionTest, SessionResolvesIndirectCallsAndReusesServices) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, IndirectIR);
  auto session = std::make_shared<AnalysisSession>(*module);
  auto graph = session->graph();
  EXPECT_EQ(graph, session->graph());
  auto aliases = session->alias_analysis();
  EXPECT_EQ(aliases, session->alias_analysis());
  EXPECT_EQ(session->type_hierarchy(), session->type_hierarchy());
  ScalarProblem problem;
  IFDSSolver<ScalarProblem> solver(problem);
  solver.set_analysis_session(session);
  solver.solve(*module);
  auto *main = module->getFunction("main");
  auto *result = lotus::unittest::findInstructionByName(*main, "result");
  EXPECT_EQ(
      solver.get_facts_at_entry(main->back().getTerminator()).count(result),
      1U);
  session->invalidate();
  EXPECT_NE(graph, session->graph());
  EXPECT_NE(aliases, session->alias_analysis());
  // Retained snapshots remain owned after invalidation.
  EXPECT_FALSE(graph->callees().empty());
  solver.solve(*module);
  EXPECT_EQ(
      solver.get_facts_at_entry(main->back().getTerminator()).count(result),
      1U);
  ScalarProblem path_problem;
  path_problem.set_analysis_session(session);
  PathAwareIFDSSolver<ScalarProblem> paths(path_problem);
  paths.solve(*module);
  EXPECT_EQ(
      paths.get_facts_at_entry(main->back().getTerminator()).count(result), 1U);
}

TEST(AnalysisSessionTest, SessionEntryPointsReplaceDefaultMainSeeds) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    define i32 @main(i32 %x) { ret i32 %x }
    define i32 @entry(i32 %x) { ret i32 %x }
    define i32 @second(i32 %x) { ret i32 %x }
  )");
  AnalysisSession::Options options;
  options.entry_points = {"entry", "second"};
  auto session = std::make_shared<AnalysisSession>(*module, options);
  ScalarProblem problem;
  IFDSSolver<ScalarProblem> solver(problem);
  solver.set_analysis_session(session);
  solver.solve(*module);
  EXPECT_TRUE(
      solver.get_facts_at_entry(&module->getFunction("main")->front().front())
          .empty());
  for (auto *name : {"entry", "second"})
    EXPECT_FALSE(
        solver.get_facts_at_entry(&module->getFunction(name)->front().front())
            .empty());
}

TEST(AnalysisSessionTest, RejectsCrossModuleServicesAndMissingEntries) {
  llvm::LLVMContext context;
  auto first = lotus::unittest::parseModuleChecked(
      context, "define void @main() { ret void }");
  auto second = lotus::unittest::parseModuleChecked(
      context, "define void @main() { ret void }");
  ScalarProblem problem;
  IFDSSolver<ScalarProblem> solver(problem);
  solver.set_analysis_session(std::make_shared<AnalysisSession>(*first));
  EXPECT_THROW(solver.solve(*second), std::invalid_argument);
  solver.set_analysis_session(nullptr);
  solver.set_icfg(
      std::make_shared<dataflow::controlflow::LLVMInterCFG>(first.get()));
  EXPECT_THROW(solver.solve(*second), std::invalid_argument);
  AnalysisSession::Options options;
  options.entry_points = {"missing"};
  EXPECT_THROW(AnalysisSession(*first, options), std::invalid_argument);
}

TEST(AnalysisSessionTest, ImmutableModelsStayIsolatedAcrossInterleavedRuns) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %value = call i32 @source()
      call void @sink(i32 %value)
      ret i32 0
    }
  )");
  auto config = TaintConfigParser::parse_string(
      "SOURCE source Ret V T\nSINK sink Arg0 V\n");
  auto active = std::make_shared<AnalysisSession>(
      *module, AnalysisSession::Options{}, config.get());
  config->sources.clear();
  config->function_specs.clear();
  TaintConfig empty;
  auto inactive = std::make_shared<AnalysisSession>(
      *module, AnalysisSession::Options{}, &empty);
  TaintAnalysis first({}, empty), second({}, empty);
  IFDSSolver<TaintAnalysis> first_solver(first), second_solver(second);
  first_solver.set_analysis_session(active);
  second_solver.set_analysis_session(inactive);
  auto *main = module->getFunction("main");
  auto *source = lotus::unittest::findInstructionByName(*main, "value");
  auto *sink = lotus::unittest::findCallTo(*main, "sink");
  first_solver.solve(*module);
  second_solver.solve(*module);
  EXPECT_EQ(first_solver.get_facts_at_entry(sink).count(
                TaintFact::tainted_var(source)),
            1U);
  EXPECT_EQ(second_solver.get_facts_at_entry(sink).count(
                TaintFact::tainted_var(source)),
            0U);
  EXPECT_TRUE(first.is_source(source));
  EXPECT_FALSE(second.is_source(source));
  first_solver.solve(*module);
  EXPECT_EQ(first_solver.get_facts_at_entry(sink).count(
                TaintFact::tainted_var(source)),
            1U);
  IDEExtendedTaintAnalysis ide_problem(empty);
  IDESolver<IDEExtendedTaintAnalysis> ide(ide_problem);
  ide.set_analysis_session(active);
  ide.solve(*module);
  EXPECT_EQ(ide.get_value_at(sink, source).kind, ExtendedTaintValue::Tainted);
}

TEST(AnalysisSessionTest, ModelsApplyToIndirectExternalSources) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %target = select i1 true, i32 ()* @source, i32 ()* @source
      %value = call i32 %target()
      call void @sink(i32 %value)
      ret i32 0
    }
  )");
  auto model = TaintConfigParser::parse_string(
      "SOURCE source Ret V T\nSINK sink Arg0 V\n");
  auto session = std::make_shared<AnalysisSession>(
      *module, AnalysisSession::Options{}, model.get());
  TaintAnalysis problem({}, *model);
  IFDSSolver<TaintAnalysis> solver(problem);
  solver.set_analysis_session(session);
  solver.solve(*module);
  auto *main = module->getFunction("main");
  auto *value = lotus::unittest::findInstructionByName(*main, "value");
  auto *sink = lotus::unittest::findCallTo(*main, "sink");
  EXPECT_TRUE(problem.is_source(value));
  EXPECT_EQ(
      solver.get_facts_at_entry(sink).count(TaintFact::tainted_var(value)), 1U);
}

TEST(AnalysisSessionTest, ExplicitGlobalAndCallbackModelsReachTheirBodies) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    @llvm.global_ctors = appending global [1 x { i32, void ()*, i8* }]
      [{ i32, void ()*, i8* } { i32 1, void ()* @ctor, i8* null }]
    declare !callback !0 void @external(void ()*)
    define void @ctor() { ret void }
    define void @callback() { ret void }
    define i32 @main() {
      call void @external(void ()* @callback)
      ret i32 0
    }
    !0 = !{!1}
    !1 = !{i64 0, i1 false}
  )");
  AnalysisSession::Options options;
  options.model_global_initializers = true;
  options.model_external_callbacks = true;
  auto session = std::make_shared<AnalysisSession>(*module, options);
  class ReachBodies : public ScalarProblem {
  public:
    FactSet initial_facts(const llvm::Function *) override { return {nullptr}; }
  };
  ReachBodies problem;
  IFDSSolver<ReachBodies> solver(problem);
  solver.set_analysis_session(session);
  solver.solve(*module);
  for (auto *name : {"main", "ctor", "callback"}) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(
        solver.get_facts_at_entry(&module->getFunction(name)->front().front())
            .empty());
  }
}

std::unique_ptr<llvm::Module> sparseFixture(llvm::LLVMContext &context) {
  return lotus::unittest::parseModuleChecked(context, R"(
    declare void @observe(i32)
    define i32 @identity(i32 %x) { ret i32 %x }
    define i32 @main(i32 %x, i1 %condition) {
      %start = add i32 0, 1
      %unused0 = add i32 2, 3
      %unused1 = mul i32 4, 5
      %used = add i32 %x, 1
      %bump = add i32 6, 7
      %unused2 = sub i32 8, 1
      br i1 %condition, label %left, label %right
    left:
      %left_first = add i32 1, 1
      %one = add i32 1, 2
      br label %merge
    right:
      %right_first = add i32 1, 1
      %two = add i32 2, 2
      br label %merge
    merge:
      %merged = phi i32 [ %one, %left ], [ %two, %right ]
      %unused3 = add i32 9, 10
      %unused4 = add i32 11, 12
      %result = call i32 @identity(i32 %used)
      call void @observe(i32 %result)
      ret i32 %result
    }
  )");
}

TEST(SparseSolverTest, IFDSMatchesEveryDenseEntryExitAndSummary) {
  llvm::LLVMContext context;
  auto module = sparseFixture(context);
  ScalarProblem dense_problem, sparse_problem;
  IFDSSolver<ScalarProblem> dense(dense_problem), sparse(sparse_problem);
  sparse.get_solver_config().set_sparse_execution();
  dense.solve(*module);
  sparse.solve(*module);
  for (const auto &function : *module)
    for (const auto &inst : llvm::instructions(function)) {
      EXPECT_EQ(dense.get_facts_at_entry(&inst),
                sparse.get_facts_at_entry(&inst));
      EXPECT_EQ(dense.get_facts_at_exit(&inst),
                sparse.get_facts_at_exit(&inst));
    }
  std::vector<SummaryEdge<const llvm::Value *>> a, b;
  dense.get_summary_edges(a);
  sparse.get_summary_edges(b);
  EXPECT_EQ(a, b);
  EXPECT_GT(sparse.get_sparse_transfers(), 0U);
  EXPECT_LT(sparse.get_steps_performed(), dense.get_steps_performed());
  sparse.solve(*module);
  EXPECT_EQ(dense.get_all_results(), sparse.get_all_results());
}

TEST(SparseSolverTest, IDEPreservesNonIdentityEdgesAndJoinedValuesAtEveryNode) {
  llvm::LLVMContext context;
  auto module = sparseFixture(context);
  ValueProblem dense_problem, sparse_problem;
  IDESolver<ValueProblem> dense(dense_problem), sparse(sparse_problem);
  sparse.get_solver_config().set_sparse_execution();
  dense.solve(*module);
  sparse.solve(*module);
  EXPECT_EQ(dense.get_all_values(), sparse.get_all_values());
  auto *main = module->getFunction("main");
  EXPECT_EQ(
      sparse.get_value_at(main->back().getTerminator(), main->getArg(0)).value,
      3U);
  EXPECT_GT(sparse.get_sparse_transfers(), 0U);
  EXPECT_LT(sparse.get_steps_performed(), dense.get_steps_performed());
  sparse.get_solver_config().set_enable_edge_function_caching(false);
  sparse.solve(*module);
  EXPECT_EQ(dense.get_all_values(), sparse.get_all_values());
}

TEST(SparseSolverTest, PathAwareKeepsImmediateEdgesAndDenseFacts) {
  llvm::LLVMContext context;
  auto module = sparseFixture(context);
  ScalarProblem a, b;
  PathAwareIFDSSolver<ScalarProblem> dense(a), sparse(b);
  sparse.get_solver_config().set_sparse_execution();
  dense.solve(*module);
  sparse.solve(*module);
  for (const auto &inst : llvm::instructions(module->getFunction("main")))
    EXPECT_EQ(dense.get_facts_at_entry(&inst),
              sparse.get_facts_at_entry(&inst));
  EXPECT_EQ(dense.get_esg().get_all_edges(), sparse.get_esg().get_all_edges());
  EXPECT_GT(sparse.get_sparse_transfers(), 0U);
}

TEST(SparseSolverTest, UnsupportedClientsFallBackToDense) {
  class Unsupported : public ScalarProblem {
  public:
    const llvm::Value *sparse_fact_value(const Fact &) const override {
      return nullptr;
    }
  };
  llvm::LLVMContext context;
  auto module = sparseFixture(context);
  Unsupported problem;
  IFDSSolver<Unsupported> solver(problem);
  solver.get_solver_config().set_sparse_execution();
  solver.solve(*module);
  EXPECT_EQ(solver.get_sparse_transfers(), 0U);
}

TEST(SparseSolverTest, ShippedTaintClientPreservesAliasedMemoryAndSinkFacts) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %p = alloca i32
      %q = getelementptr i32, i32* %p, i64 0
      %value = call i32 @source()
      %unused0 = add i32 2, 3
      %unused1 = mul i32 4, 5
      store i32 %value, i32* %p
      %loaded = load i32, i32* %q
      call void @sink(i32 %loaded)
      ret i32 0
    }
  )");
  TaintConfig model;
  model.sources = {"source"};
  model.sinks = {"sink"};
  auto session = std::make_shared<AnalysisSession>(
      *module, AnalysisSession::Options{}, &model);
  TaintAnalysis a({}, model), b({}, model);
  IFDSSolver<TaintAnalysis> dense(a), sparse(b);
  dense.set_analysis_session(session);
  sparse.set_analysis_session(session);
  sparse.get_solver_config().set_sparse_execution();
  dense.solve(*module);
  sparse.solve(*module);
  EXPECT_EQ(dense.get_all_results(), sparse.get_all_results());
  auto *main = module->getFunction("main");
  auto *loaded = lotus::unittest::findInstructionByName(*main, "loaded");
  auto *sink = lotus::unittest::findCallTo(*main, "sink");
  EXPECT_EQ(
      sparse.get_facts_at_entry(sink).count(TaintFact::tainted_var(loaded)),
      1U);
  EXPECT_GT(sparse.get_sparse_transfers(), 0U);
}

} // namespace
} // namespace ifds
