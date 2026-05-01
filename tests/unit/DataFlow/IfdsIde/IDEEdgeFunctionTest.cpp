#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Solver/IDESolver.h>
#include <Dataflow/IFDS/Support/EdgeFunctionUtils.h>
#include <TestUtils/LLVMHelpers.h>
#include <algorithm>

namespace ifds {
namespace {

// Minimal IDEProblem implementation just to test IDEProblem::compose/identity.
class DummyIDEProblem : public IDEProblem<int, int> {
public:
  using Fact = int;
  using Value = int;

  Fact zero_fact() const override { return 0; }

  FactSet normal_flow(const llvm::Instruction * /*stmt*/,
                      const llvm::Instruction * /*succ*/,
                      const Fact &fact) override {
    return {fact};
  }
  FactSet call_flow(const llvm::CallBase * /*call*/,
                    const llvm::Function * /*callee*/,
                    const Fact &fact) override {
    return {fact};
  }
  FactSet return_flow(const llvm::CallBase * /*call*/,
                      const llvm::Instruction * /*exit_inst*/,
                      const llvm::Instruction * /*return_site*/,
                      const llvm::Function * /*callee*/, const Fact &exit_fact,
                      const Fact & /*call_fact*/) override {
    return {exit_fact};
  }
  FactSet
  call_to_return_flow(const llvm::CallBase * /*call*/,
                      const llvm::Instruction * /*return_site*/,
                      llvm::ArrayRef<const llvm::Function *> /*callees*/,
                      const Fact &fact) override {
    return {fact};
  }
  FactSet initial_facts(const llvm::Function * /*main*/) override { return {}; }
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    return this->lift_ifds_initial_seeds(module, bottom_value());
  }

  Value top_value() const override { return 0; }
  Value bottom_value() const override { return 0; }
  Value join(const Value & /*v1*/, const Value &v2) const override {
    return v2;
  }

  EdgeFunction normal_edge_function(const llvm::Instruction * /*stmt*/,
                                    const llvm::Instruction * /*succ*/,
                                    const Fact & /*src_fact*/,
                                    const Fact & /*tgt_fact*/) override {
    return identity();
  }
  EdgeFunction call_edge_function(const llvm::CallBase * /*call*/,
                                  const llvm::Function * /*callee*/,
                                  const Fact & /*src_fact*/,
                                  const Fact & /*tgt_fact*/) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase * /*call*/,
                                    const llvm::Function * /*callee*/,
                                    const llvm::Instruction * /*exit_inst*/,
                                    const llvm::Instruction * /*return_site*/,
                                    const Fact & /*exit_fact*/,
                                    const Fact & /*ret_fact*/) override {
    return identity();
  }
  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase * /*call*/,
      const llvm::Instruction * /*return_site*/,
      llvm::ArrayRef<const llvm::Function *> /*callees*/,
      const Fact & /*src_fact*/, const Fact & /*tgt_fact*/) override {
    return identity();
  }
};

class SeedValueProblem : public DummyIDEProblem {
public:
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    IDEInitialSeeds seeds;
    auto *main = module.getFunction("main");
    auto *entry = main == nullptr || main->empty()
                      ? nullptr
                      : &main->getEntryBlock().front();
    if (entry != nullptr) {
      seeds.add_seed(entry, zero_fact(), 42);
    }
    return seeds;
  }
};

TEST(IDEEdgeFunctionTest, IdentityIsNeutral) {
  DummyIDEProblem P;
  auto id = P.identity();
  EXPECT_EQ(id(0), 0);
  EXPECT_EQ(id(7), 7);
  EXPECT_EQ(id(-3), -3);
}

TEST(IDEEdgeFunctionTest, ComposeOrderMatchesSolverUsage) {
  // In lotus, compose(f1, f2) is implemented as f1(f2(v)).
  // This mirrors the solver's "new_phi = compose(edge_fn, phi)" usage.
  DummyIDEProblem P;

  auto add2 = [](int x) { return x + 2; };
  auto mul2 = [](int x) { return x * 2; };

  // Expected: add2(mul2(3)) == 8
  auto addAfterMul = P.compose(add2, mul2);
  EXPECT_EQ(addAfterMul(3), 8);

  // Expected: mul2(add2(3)) == 10
  auto mulAfterAdd = P.compose(mul2, add2);
  EXPECT_EQ(mulAfterAdd(3), 10);

  // Recreate PhASAR's ((3 + 2) * 2) + 2 == 12 with explicit composition.
  // Step1: v -> v + 2
  // Step2: v -> v * 2
  // Step3: v -> v + 2
  auto addThenMulThenAdd = P.compose(add2, P.compose(mul2, add2));
  EXPECT_EQ(addThenMulThenAdd(3), 12);
}

TEST(IDEEdgeFunctionTest, EquivalenceIsConservativeWhenTopEqualsBottom) {
  DummyIDEProblem P;
  auto plus1 = [](int x) { return x + 1; };
  auto plus2 = [](int x) { return x + 2; };
  EXPECT_FALSE(P.edge_function_equivalent(plus1, plus2));
}

TEST(IDEEdgeFunctionTest, ExplicitSeedValuesPropagate) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModule(Ctx, R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )", "IDEEdgeFunctionTest");
  auto *Ret = M->getFunction("main")->back().getTerminator();

  SeedValueProblem Problem;
  IDESolver<SeedValueProblem> Solver(Problem);
  Solver.solve(*M);

  EXPECT_EQ(Solver.get_value_at(Ret, 0), 42);
}

TEST(IDEEdgeFunctionTest, FirstClassEdgeFunctionsExposeStructure) {
  auto const7 = edge::constant<int>(7);
  DummyIDEProblem::EdgeFunction as_problem_edge_function = const7;

  EXPECT_EQ(as_problem_edge_function(0), 7);
  EXPECT_EQ(as_problem_edge_function(42), 7);
  EXPECT_EQ(const7.kind(), edge::EdgeFunctionKind::Constant);
  ASSERT_TRUE(const7.constant().has_value());
  EXPECT_EQ(*const7.constant(), 7);
  EXPECT_TRUE(const7.structurally_equal(edge::constant<int>(7)));
  EXPECT_FALSE(const7.structurally_equal(edge::constant<int>(8)));
}

TEST(IDEEdgeFunctionTest, FirstClassEdgeFunctionCompositionPreservesOrder) {
  auto add2 = edge::lambda<int>("add2", [](int value) { return value + 2; });
  auto mul3 = edge::lambda<int>("mul3", [](int value) { return value * 3; });

  auto composed = edge::compose<int>(add2, mul3);
  EXPECT_EQ(composed(4), 14); // add2(mul3(4))

  auto id = edge::identity<int>();
  EXPECT_EQ(edge::compose<int>(id, add2)(4), add2(4));
  EXPECT_EQ(edge::compose<int>(mul3, id)(4), mul3(4));
}

TEST(IDEEdgeFunctionTest, FirstClassEdgeFunctionJoinUsesValueJoin) {
  auto const3 = edge::constant<int>(3);
  auto const5 = edge::constant<int>(5);
  auto joined = edge::join<int>(const3, const5,
                                [](int left, int right) {
                                  return std::max(left, right);
                                });

  EXPECT_EQ(joined(0), 5);
  EXPECT_TRUE(edge::join<int>(const3, const3, [](int left, int right) {
                return std::max(left, right);
              }).structurally_equal(const3));
  EXPECT_EQ(joined.kind(), edge::EdgeFunctionKind::Constant);
}

TEST(IDEEdgeFunctionTest, AllTopAndAllBottomIgnoreInput) {
  auto all_top = edge::all_top<int>(99);
  auto all_bottom = edge::all_bottom<int>(-5);

  EXPECT_EQ(all_top.kind(), edge::EdgeFunctionKind::AllTop);
  EXPECT_EQ(all_bottom.kind(), edge::EdgeFunctionKind::AllBottom);
  EXPECT_EQ(all_top(1), 99);
  EXPECT_EQ(all_top(100), 99);
  EXPECT_EQ(all_bottom(1), -5);
  EXPECT_EQ(all_bottom(100), -5);
}

TEST(IDEEdgeFunctionTest, AllTopAndAllBottomAreStructurallyComparable) {
  auto top_a = edge::all_top<int>(7);
  auto top_b = edge::all_top<int>(7);
  auto top_c = edge::all_top<int>(8);
  auto bottom_a = edge::all_bottom<int>(1);
  auto bottom_b = edge::all_bottom<int>(1);

  EXPECT_TRUE(top_a.structurally_equal(top_b));
  EXPECT_FALSE(top_a.structurally_equal(top_c));
  EXPECT_TRUE(bottom_a.structurally_equal(bottom_b));
  EXPECT_FALSE(top_a.structurally_equal(bottom_a));
}

TEST(IDEEdgeFunctionTest, ComposeSimplifiesNewKinds) {
  auto add2 = edge::lambda<int>("add2", [](int value) { return value + 2; });
  auto all_top = edge::all_top<int>(10);
  auto all_bottom = edge::all_bottom<int>(-1);
  auto id = edge::identity<int>();

  auto top_after_add = edge::compose<int>(all_top, add2);
  auto bottom_after_add = edge::compose<int>(all_bottom, add2);

  EXPECT_TRUE(top_after_add.structurally_equal(all_top));
  EXPECT_TRUE(bottom_after_add.structurally_equal(all_bottom));
  EXPECT_TRUE(edge::compose<int>(id, all_top).structurally_equal(all_top));
  EXPECT_TRUE(edge::compose<int>(all_bottom, id).structurally_equal(all_bottom));
}

TEST(IDEEdgeFunctionTest, JoinSimplifiesNewKinds) {
  auto const3 = edge::constant<int>(3);
  auto const5 = edge::constant<int>(5);
  auto all_top = edge::all_top<int>(99);
  auto all_bottom = edge::all_bottom<int>(-1);

  auto join_values = [](int left, int right) { return std::max(left, right); };

  EXPECT_TRUE(edge::join<int>(all_top, const3, join_values)
                  .structurally_equal(all_top));
  EXPECT_TRUE(edge::join<int>(const3, all_top, join_values)
                  .structurally_equal(all_top));
  EXPECT_TRUE(edge::join<int>(all_bottom, const3, join_values)
                  .structurally_equal(const3));
  EXPECT_TRUE(edge::join<int>(const3, all_bottom, join_values)
                  .structurally_equal(const3));

  auto joined_constants = edge::join<int>(const3, const5, join_values);
  EXPECT_EQ(joined_constants.kind(), edge::EdgeFunctionKind::Constant);
  ASSERT_TRUE(joined_constants.constant().has_value());
  EXPECT_EQ(*joined_constants.constant(), 5);
}

TEST(IDEEdgeFunctionTest, StructuredJoinTracksOperands) {
  auto add1 = edge::lambda<int>("add1", [](int value) { return value + 1; });
  auto mul2 = edge::lambda<int>("mul2", [](int value) { return value * 2; });
  auto joined = edge::join<int>(add1, mul2,
                                [](int left, int right) {
                                  return std::max(left, right);
                                });

  EXPECT_EQ(joined.kind(), edge::EdgeFunctionKind::Join);
  ASSERT_TRUE(joined.left() != nullptr);
  ASSERT_TRUE(joined.right() != nullptr);
  EXPECT_EQ(joined.left()->kind(), edge::EdgeFunctionKind::Lambda);
  EXPECT_EQ(joined.right()->kind(), edge::EdgeFunctionKind::Lambda);
  EXPECT_EQ(joined.left()->name(), add1.name());
  EXPECT_EQ(joined.right()->name(), mul2.name());
  EXPECT_EQ((*joined.left())(4), add1(4));
  EXPECT_EQ((*joined.right())(4), mul2(4));
  EXPECT_EQ(joined(4), 8);
}

TEST(IDEEdgeFunctionTest, ProblemConvenienceFactoriesUseDomainExtrema) {
  DummyIDEProblem P;
  auto top = P.all_top();
  auto bottom = P.all_bottom();

  EXPECT_EQ(top.kind(), edge::EdgeFunctionKind::AllTop);
  EXPECT_EQ(bottom.kind(), edge::EdgeFunctionKind::AllBottom);
  EXPECT_EQ(top(123), P.top_value());
  EXPECT_EQ(bottom(123), P.bottom_value());
}

TEST(IDEEdgeFunctionTest, LambdasAreNotStructurallyEquivalentByName) {
  DummyIDEProblem P;
  auto left = edge::lambda<int>("same-name", [](int value) { return value + 1; });
  auto right = edge::lambda<int>("same-name", [](int value) { return value + 1; });

  EXPECT_FALSE(left.structurally_equal(right));
  EXPECT_FALSE(P.edge_function_equivalent(left, right));
}

TEST(IDEEdgeFunctionTest, ConstantFunctionsAreStructurallyEquivalent) {
  DummyIDEProblem P;
  auto left = edge::constant<int>(11);
  auto right = edge::constant<int>(11);
  auto different = edge::constant<int>(12);

  EXPECT_TRUE(P.edge_function_equivalent(left, right));
  EXPECT_FALSE(P.edge_function_equivalent(left, different));
}

} // namespace
} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
