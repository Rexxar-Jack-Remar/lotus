#include "Alias/InclusionBased/GPG/Analysis.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "TestUtils/LLVMHelpers.h"

#include <set>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

using namespace lotus::gpg;
using namespace lotus::unittest;

TEST(GPGWrapperRegression, AbstractPointeesDoNotProduceStrongAliasAnswers) {
  const char *ir = R"(
    @a = global i8 0
    @b = global i8 0

    define void @main(i1 %condition) {
    entry:
      %unknown = select i1 %condition, i8* @a, i8* undef
      %different = select i1 %condition, i8* @b, i8* @b
      %nullable = select i1 %condition, i8* @a, i8* null
      %same_first = select i1 %condition, i8* @a, i8* @a
      %same_second = select i1 %condition, i8* @a, i8* @a
      ret void
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGWrapperRegression");
  ASSERT_NE(module, nullptr);
  llvm::Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  llvm::Instruction *unknown = findInstructionByName(main, "unknown");
  llvm::Instruction *different = findInstructionByName(main, "different");
  llvm::Instruction *nullable = findInstructionByName(main, "nullable");
  llvm::Instruction *same_first = findInstructionByName(main, "same_first");
  llvm::Instruction *same_second = findInstructionByName(main, "same_second");
  ASSERT_NE(unknown, nullptr);
  ASSERT_NE(different, nullptr);
  ASSERT_NE(nullable, nullptr);
  ASSERT_NE(same_first, nullptr);
  ASSERT_NE(same_second, nullptr);

  GPGAnalysisEngine analysis(*module);
  analysis.run();

  const PointeeSetResult unknown_result =
      analysis.result().allPointeeSet(unknown);
  EXPECT_FALSE(unknown_result.isComplete());
  EXPECT_TRUE(unknown_result.containsUnknown());
  EXPECT_FALSE(unknown_result.containsNull());
  EXPECT_EQ(unknown_result.values,
            (std::set<const llvm::Value *>{module->getGlobalVariable("a")}));
  const PointeeSetResult unknown_at_definition =
      analysis.result().pointeeSet(unknown, unknown);
  EXPECT_FALSE(unknown_at_definition.isComplete());
  EXPECT_TRUE(unknown_at_definition.containsUnknown());

  const PointeeSetResult nullable_result =
      analysis.result().allPointeeSet(nullable);
  EXPECT_TRUE(nullable_result.isComplete());
  EXPECT_FALSE(nullable_result.containsUnknown());
  EXPECT_TRUE(nullable_result.containsNull());
  EXPECT_EQ(nullable_result.values,
            (std::set<const llvm::Value *>{module->getGlobalVariable("a")}));
  const PointeeSetResult nullable_at_definition =
      analysis.result().pointeeSet(nullable, nullable);
  EXPECT_TRUE(nullable_at_definition.isComplete());
  EXPECT_TRUE(nullable_at_definition.containsNull());

  const PointeeSetResult concrete_result =
      analysis.result().allPointeeSet(same_first);
  EXPECT_TRUE(concrete_result.isComplete());
  EXPECT_FALSE(concrete_result.containsUnknown());
  EXPECT_FALSE(concrete_result.containsNull());

  lotus::AliasAnalysisWrapper wrapper(*module, lotus::AAConfig::GPG());
  ASSERT_TRUE(wrapper.isInitialized());
  EXPECT_EQ(wrapper.query(unknown, different), llvm::AliasResult::MayAlias);
  EXPECT_EQ(wrapper.query(nullable, same_first), llvm::AliasResult::MayAlias);
  EXPECT_EQ(wrapper.query(same_first, different), llvm::AliasResult::NoAlias);
  EXPECT_EQ(wrapper.query(same_first, same_second),
            llvm::AliasResult::MustAlias);

  std::vector<const llvm::Value *> points_to;
  EXPECT_FALSE(wrapper.getPointsToSet(unknown, points_to));
  EXPECT_TRUE(points_to.empty());
  EXPECT_FALSE(wrapper.getPointsToSet(nullable, points_to));
  EXPECT_TRUE(points_to.empty());
  EXPECT_TRUE(wrapper.getPointsToSet(same_first, points_to));
  EXPECT_EQ(points_to,
            (std::vector<const llvm::Value *>{module->getGlobalVariable("a")}));

  std::size_t size = 0;
  EXPECT_FALSE(wrapper.getPointsToSetSize(nullable, size));
  EXPECT_TRUE(wrapper.getPointsToSetSize(same_first, size));
  EXPECT_EQ(size, 1u);
}
