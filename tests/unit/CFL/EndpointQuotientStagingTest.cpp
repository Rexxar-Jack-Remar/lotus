#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotientStaging.h"

#include <algorithm>
#include <stdexcept>

#include <gtest/gtest.h>

namespace lotus::cfl::endpoint {
namespace {

TEST(EndpointQuotientStagingTest, OrdersDependenciesBeforeConsumers) {
  Problem problem;
  problem.symbols = 6;
  // The deliberately shuffled rules form {1,2} -> 0 -> 4. Symbol 3 and 5
  // are independent, so the smallest available symbol breaks each tie.
  problem.rules = {
      Rule::unary(4, 0),
      Rule::unary(2, 1),
      Rule::unary(0, 2),
      Rule::unary(1, 2),
  };

  const StagingPlan plan = buildStagingPlan(problem);
  ASSERT_EQ(plan.stages.size(), 5U);
  EXPECT_LT(plan.symbol_to_stage[1], plan.symbol_to_stage[0]);
  EXPECT_LT(plan.symbol_to_stage[0], plan.symbol_to_stage[4]);
  EXPECT_EQ(plan.symbol_to_stage[1], plan.symbol_to_stage[2]);
  EXPECT_EQ(plan.stageForSymbol(1).kind, SccKind::UnaryRegular);

  for (const StagingStage &stage : plan.stages)
    for (Id dependency : stage.dependencies)
      EXPECT_LT(dependency, stage.index);
}

TEST(EndpointQuotientStagingTest, ClassifiesConservativeStageShapes) {
  Problem problem;
  problem.symbols = 12;
  problem.rules = {
      Rule::unary(0, 1), // acyclic
      Rule::unary(2, 3), // unary-regular mutual recursion
      Rule::unary(3, 2),
      Rule::binary(4, 4, 4), // transitive-self
      Rule::unary(4, 4),     // harmless identity recursion
      Rule::binary(5, 5, 1), // mixed-direction singleton recursion
      Rule::binary(5, 1, 5),
      Rule::binary(6, 7, 1), // nonlinear mutual recursion
      Rule::unary(7, 6),
      Rule::binary(8, 8, 1), // singleton left-linear recursion
      Rule::unary(8, 8),
      Rule::binary(9, 1, 9),   // singleton right-linear recursion
      Rule::binary(10, 10, 1), // mixing both sides is conservatively general
      Rule::binary(10, 1, 10),
      Rule::binary(11, 11, 1), // multiple recursive symbols stay general
      Rule::unary(1, 11),
  };

  const StagingPlan plan = buildStagingPlan(problem);
  EXPECT_EQ(plan.stageForSymbol(0).kind, SccKind::Acyclic);
  EXPECT_EQ(plan.stageForSymbol(2).kind, SccKind::UnaryRegular);
  EXPECT_EQ(plan.stageForSymbol(4).kind, SccKind::TransitiveSelf);
  EXPECT_EQ(plan.stageForSymbol(5).kind, SccKind::General);
  EXPECT_EQ(plan.stageForSymbol(6).kind, SccKind::General);
  EXPECT_EQ(plan.stageForSymbol(7).kind, SccKind::General);
  EXPECT_EQ(plan.stageForSymbol(8).kind, SccKind::LeftLinear);
  EXPECT_EQ(plan.stageForSymbol(9).kind, SccKind::RightLinear);
  EXPECT_EQ(plan.stageForSymbol(10).kind, SccKind::General);
  EXPECT_EQ(plan.stageForSymbol(11).kind, SccKind::General);
}

TEST(EndpointQuotientStagingTest, IsIndependentOfRuleOrderAndDuplicates) {
  Problem first;
  first.symbols = 4;
  first.rules = {Rule::unary(3, 2), Rule::binary(2, 0, 1),
                 Rule::binary(2, 0, 1), Rule::epsilon(0)};
  Problem second = first;
  std::reverse(second.rules.begin(), second.rules.end());

  const StagingPlan lhs = buildStagingPlan(first);
  const StagingPlan rhs = buildStagingPlan(second);
  ASSERT_EQ(lhs.stages.size(), rhs.stages.size());
  EXPECT_EQ(lhs.symbol_to_stage, rhs.symbol_to_stage);
  for (Id index = 0; index < lhs.stages.size(); ++index) {
    EXPECT_EQ(lhs.stages[index].symbols, rhs.stages[index].symbols);
    EXPECT_EQ(lhs.stages[index].dependencies, rhs.stages[index].dependencies);
    EXPECT_EQ(lhs.stages[index].rules.size(), rhs.stages[index].rules.size());
  }
  EXPECT_EQ(lhs.stageForSymbol(2).rules.size(), 1U);
}

TEST(EndpointQuotientStagingTest, RejectsOutOfRangeSymbols) {
  Problem problem;
  problem.symbols = 1;
  problem.rules = {Rule::unary(0, 1)};
  EXPECT_THROW(buildStagingPlan(problem), std::invalid_argument);
}

} // namespace
} // namespace lotus::cfl::endpoint
