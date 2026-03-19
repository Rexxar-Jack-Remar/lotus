#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>
#include <unordered_set>

namespace ifds {
namespace {

struct SeedFact {
  int value = 0;

  bool operator==(const SeedFact &other) const { return value == other.value; }
  bool operator!=(const SeedFact &other) const { return !(*this == other); }
  bool operator<(const SeedFact &other) const { return value < other.value; }
};

} // namespace
} // namespace ifds

namespace std {
template <> struct hash<ifds::SeedFact> {
  size_t operator()(const ifds::SeedFact &fact) const {
    return std::hash<int>{}(fact.value);
  }
};
} // namespace std

namespace ifds {
namespace {

class SeededCalleeProblem : public IFDSProblem<SeedFact> {
public:
  SeedFact zero_fact() const override { return SeedFact{0}; }

  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const SeedFact &fact) override {
    if (fact.value == 0 && !llvm::isa<llvm::ReturnInst>(stmt)) {
      return {};
    }
    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const SeedFact &fact) override {
    return {fact};
  }

  FactSet return_flow(const llvm::CallBase *, const llvm::Instruction *,
                      const llvm::Instruction *, const llvm::Function *,
                      const SeedFact &exit_fact,
                      const SeedFact &) override {
    return {exit_fact};
  }

  FactSet call_to_return_flow(const llvm::CallBase *,
                              const llvm::Instruction *,
                              llvm::ArrayRef<const llvm::Function *>,
                              const SeedFact &) override {
    return {};
  }

  FactSet initial_facts(const llvm::Function *) override { return {}; }

  InitialSeeds initial_seeds(const llvm::Module &module) override {
    InitialSeeds seeds;
    auto *seeded = module.getFunction("seeded");
    if (!seeded || seeded->empty()) {
      return seeds;
    }
    seeds.add_seed(&seeded->getEntryBlock().front(), SeedFact{1});
    return seeds;
  }
};

class IFDSSolverInvariantsTest : public ::testing::Test {
protected:
  std::unique_ptr<llvm::LLVMContext> ctx = std::make_unique<llvm::LLVMContext>();

  std::unique_ptr<llvm::Module> createLoopModule() {
    auto m = std::make_unique<llvm::Module>("ifds_invariants", *ctx);
    auto *i32 = llvm::Type::getInt32Ty(*ctx);
    auto *mainTy = llvm::FunctionType::get(i32, {}, false);
    auto *sourceTy = llvm::FunctionType::get(i32, {}, false);
    auto *sinkTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx), {i32}, false);

    auto *mainFn =
        llvm::Function::Create(mainTy, llvm::Function::ExternalLinkage, "main", m.get());
    auto *sourceFn = llvm::Function::Create(sourceTy, llvm::Function::ExternalLinkage,
                                            "source", m.get());
    auto *sinkFn = llvm::Function::Create(sinkTy, llvm::Function::ExternalLinkage, "sink",
                                          m.get());

    auto *entry = llvm::BasicBlock::Create(*ctx, "entry", mainFn);
    auto *body = llvm::BasicBlock::Create(*ctx, "body", mainFn);
    auto *exit = llvm::BasicBlock::Create(*ctx, "exit", mainFn);

    llvm::IRBuilder<> be(entry);
    auto *seed = be.CreateCall(sourceTy, sourceFn, {});
    auto *cond = be.CreateICmpEQ(seed, llvm::ConstantInt::get(i32, 0));
    be.CreateCondBr(cond, body, exit);

    llvm::IRBuilder<> bb(body);
    bb.CreateCall(sinkTy, sinkFn, {seed});
    bb.CreateBr(exit);

    llvm::IRBuilder<> bx(exit);
    bx.CreateRet(llvm::ConstantInt::get(i32, 0));

    return m;
  }

  std::unique_ptr<llvm::Module> createSeededCalleeModule() {
    auto m = std::make_unique<llvm::Module>("ifds_seeded_callee", *ctx);
    auto *i32 = llvm::Type::getInt32Ty(*ctx);
    auto *calleeTy = llvm::FunctionType::get(i32, {}, false);
    auto *mainTy = llvm::FunctionType::get(i32, {}, false);

    auto *callee = llvm::Function::Create(calleeTy, llvm::Function::InternalLinkage,
                                          "seeded", m.get());
    auto *mainFn =
        llvm::Function::Create(mainTy, llvm::Function::ExternalLinkage, "main", m.get());

    auto *calleeEntry = llvm::BasicBlock::Create(*ctx, "entry", callee);
    llvm::IRBuilder<> cb(calleeEntry);
    auto *tmp = cb.CreateAdd(llvm::ConstantInt::get(i32, 1),
                             llvm::ConstantInt::get(i32, 2), "tmp");
    cb.CreateRet(tmp);

    auto *entry = llvm::BasicBlock::Create(*ctx, "entry", mainFn);
    llvm::IRBuilder<> b(entry);
    b.CreateCall(calleeTy, callee, {});
    b.CreateRet(llvm::ConstantInt::get(i32, 0));

    return m;
  }
};

TEST_F(IFDSSolverInvariantsTest, PathEdgesAreUnique) {
  auto m = createLoopModule();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver(analysis);
  solver.solve(*m);

  std::vector<PathEdge<TaintFact>> path_edges;
  solver.get_path_edges(path_edges);
  std::unordered_set<PathEdge<TaintFact>, PathEdgeHash<TaintFact>> uniq;
  for (const auto &edge : path_edges) {
    uniq.insert(edge);
  }
  EXPECT_EQ(uniq.size(), path_edges.size());
}

TEST_F(IFDSSolverInvariantsTest, StepBoundIsDeterministic) {
  auto m = createLoopModule();
  TaintAnalysis analysis;
  analysis.add_source_function("source");
  analysis.add_sink_function("sink");

  IFDSSolver<TaintAnalysis> solver_a(analysis);
  solver_a.set_max_steps(4);
  solver_a.solve(*m);
  std::vector<PathEdge<TaintFact>> a_edges;
  solver_a.get_path_edges(a_edges);

  TaintAnalysis analysis_b;
  analysis_b.add_source_function("source");
  analysis_b.add_sink_function("sink");
  IFDSSolver<TaintAnalysis> solver_b(analysis_b);
  solver_b.set_max_steps(4);
  solver_b.solve(*m);
  std::vector<PathEdge<TaintFact>> b_edges;
  solver_b.get_path_edges(b_edges);

  EXPECT_TRUE(solver_a.bound_reached());
  EXPECT_TRUE(solver_b.bound_reached());
  EXPECT_EQ(a_edges.size(), b_edges.size());
}

TEST_F(IFDSSolverInvariantsTest, UnbalancedSeededReturnsDoNotSynthesizeZero) {
  auto m = createSeededCalleeModule();
  auto *mainFn = m->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const llvm::Instruction *ret_site = mainFn->getEntryBlock().getTerminator();

  SeededCalleeProblem problem;
  IFDSSolver<SeededCalleeProblem> solver(problem);
  auto config = solver.get_solver_config();
  config.set_follow_returns_past_seeds(true);
  solver.set_solver_config(config);
  solver.solve(*m);

  auto facts = solver.get_facts_at_entry(ret_site);
  EXPECT_TRUE(facts.count(SeedFact{1}) > 0);
  EXPECT_EQ(facts.count(problem.zero_fact()), 0U);
}

} // namespace
} // namespace ifds

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
