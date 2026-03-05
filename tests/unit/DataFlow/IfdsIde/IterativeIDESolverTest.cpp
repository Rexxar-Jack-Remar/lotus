#include <Dataflow/IFDS/Solvers/IterativeIDESolver.h>
#include <gtest/gtest.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <algorithm>

namespace ifds {
namespace {

class MinimalIDEProblem : public IDEProblem<const llvm::Value *, int> {
public:
  using Fact = const llvm::Value *;
  using Value = int;

  Fact zero_fact() const override { return nullptr; }
  FactSet normal_flow(const llvm::Instruction *, const Fact &fact) override {
    return {fact};
  }
  FactSet call_flow(const llvm::CallBase *, const llvm::Function *,
                    const Fact &fact) override {
    return {fact};
  }
  FactSet return_flow(const llvm::CallBase *, const llvm::Function *,
                      const Fact &exit_fact, const Fact &) override {
    return {exit_fact};
  }
  FactSet call_to_return_flow(const llvm::CallBase *, const Fact &fact) override {
    return {fact};
  }
  FactSet initial_facts(const llvm::Function *) override { return {zero_fact()}; }

  Value top_value() const override { return 0; }
  Value bottom_value() const override { return 0; }
  Value join(const Value &v1, const Value &v2) const override {
    return std::max(v1, v2);
  }

  EdgeFunction normal_edge_function(const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    return identity();
  }
  EdgeFunction call_edge_function(const llvm::CallBase *, const Fact &,
                                  const Fact &) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase *, const Fact &,
                                    const Fact &) override {
    return identity();
  }
  EdgeFunction call_to_return_edge_function(const llvm::CallBase *, const Fact &,
                                            const Fact &) override {
    return identity();
  }
};

class IterativeIDESolverTest : public ::testing::Test {
protected:
  std::unique_ptr<llvm::LLVMContext> ctx = std::make_unique<llvm::LLVMContext>();

  std::unique_ptr<llvm::Module> createCallChainModule() {
    auto m = std::make_unique<llvm::Module>("iter_call_chain", *ctx);
    auto *i32 = llvm::Type::getInt32Ty(*ctx);
    auto *fnTy = llvm::FunctionType::get(i32, {}, false);

    auto *bar = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "bar", m.get());
    auto *foo = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "foo", m.get());
    auto *mainFn =
        llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "main", m.get());

    {
      auto *bb = llvm::BasicBlock::Create(*ctx, "entry", bar);
      llvm::IRBuilder<> b(bb);
      b.CreateRet(llvm::ConstantInt::get(i32, 1));
    }
    {
      auto *bb = llvm::BasicBlock::Create(*ctx, "entry", foo);
      llvm::IRBuilder<> b(bb);
      auto *v = b.CreateCall(fnTy, bar, {});
      b.CreateRet(v);
    }
    {
      auto *bb = llvm::BasicBlock::Create(*ctx, "entry", mainFn);
      llvm::IRBuilder<> b(bb);
      auto *v = b.CreateCall(fnTy, foo, {});
      b.CreateRet(v);
    }
    return m;
  }

  std::unique_ptr<llvm::Module> createConstModule(int literal) {
    auto m = std::make_unique<llvm::Module>("iter_const", *ctx);
    auto *i32 = llvm::Type::getInt32Ty(*ctx);
    auto *fnTy = llvm::FunctionType::get(i32, {i32}, false);
    auto *foo = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "foo", m.get());
    auto *bb = llvm::BasicBlock::Create(*ctx, "entry", foo);
    llvm::IRBuilder<> b(bb);
    auto *sum = b.CreateAdd(foo->getArg(0), llvm::ConstantInt::get(i32, literal));
    b.CreateRet(sum);
    return m;
  }
};

TEST_F(IterativeIDESolverTest, NoChangeRunReusesCachedResults) {
  auto m = createCallChainModule();
  MinimalIDEProblem problem;
  IterativeIDESolver<MinimalIDEProblem> solver(problem);
  solver.solve_full(*m);
  auto first = solver.get_stats();

  solver.solve_incremental(*m, {});
  auto second = solver.get_stats();

  EXPECT_EQ(first.num_iterations + 1, second.num_iterations);
  EXPECT_GE(second.num_reused_results, 1u);
  EXPECT_EQ(second.num_reanalyzed_functions, 0u);
}

TEST_F(IterativeIDESolverTest, ChangedFunctionFrontierExpandsToCallersAndCallees) {
  auto m = createCallChainModule();
  MinimalIDEProblem problem;
  IterativeIDESolver<MinimalIDEProblem> solver(problem);
  solver.solve_full(*m);
  solver.solve_incremental(*m, {"bar"});
  auto stats = solver.get_stats();

  EXPECT_GE(stats.num_reanalyzed_functions, 3u);
}

TEST_F(IterativeIDESolverTest, DetectsLiteralOnlyChanges) {
  auto old_m = createConstModule(1);
  auto new_m = createConstModule(2);
  ModuleVersionTracker tracker;

  auto old_v = tracker.snapshot(*old_m);
  auto new_v = tracker.snapshot(*new_m);
  auto changed = tracker.detect_changes(old_v, new_v);

  EXPECT_FALSE(changed.empty());
  EXPECT_NE(std::find(changed.begin(), changed.end(), "foo"), changed.end());
}

} // namespace
} // namespace ifds

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
