#include <Dataflow/IFDS/Solvers/IDESolver.h>
#include <gtest/gtest.h>

namespace ifds {
namespace {

// Minimal IDEProblem implementation just to test IDEProblem::compose/identity.
class DummyIDEProblem : public IDEProblem<int, int> {
public:
  using Fact = int;
  using Value = int;

  Fact zero_fact() const override { return 0; }

  FactSet normal_flow(const llvm::Instruction* /*stmt*/,
                      const Fact& fact) override {
    return {fact};
  }
  FactSet call_flow(const llvm::CallBase* /*call*/,
                    const llvm::Function* /*callee*/,
                    const Fact& fact) override {
    return {fact};
  }
  FactSet return_flow(const llvm::CallBase* /*call*/,
                      const llvm::Function* /*callee*/,
                      const Fact& exit_fact,
                      const Fact& /*call_fact*/) override {
    return {exit_fact};
  }
  FactSet call_to_return_flow(const llvm::CallBase* /*call*/,
                              const Fact& fact) override {
    return {fact};
  }
  FactSet initial_facts(const llvm::Function* /*main*/) override { return {}; }

  Value top_value() const override { return 0; }
  Value bottom_value() const override { return 0; }
  Value join(const Value& /*v1*/, const Value& v2) const override { return v2; }

  EdgeFunction normal_edge_function(const llvm::Instruction* /*stmt*/,
                                    const Fact& /*src_fact*/,
                                    const Fact& /*tgt_fact*/) override {
    return identity();
  }
  EdgeFunction call_edge_function(const llvm::CallBase* /*call*/,
                                  const Fact& /*src_fact*/,
                                  const Fact& /*tgt_fact*/) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase* /*call*/,
                                    const Fact& /*exit_fact*/,
                                    const Fact& /*ret_fact*/) override {
    return identity();
  }
  EdgeFunction call_to_return_edge_function(const llvm::CallBase* /*call*/,
                                            const Fact& /*src_fact*/,
                                            const Fact& /*tgt_fact*/) override {
    return identity();
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

} // namespace
} // namespace ifds

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

