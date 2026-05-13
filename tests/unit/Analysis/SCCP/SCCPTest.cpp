#include "Analysis/SCCP/SCCP.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>

namespace {

using lotus::analysis::sccp::SccpValue;
using lotus::analysis::sccp::runSCCPOnFunction;
using lotus::analysis::sccp::runSCCPOnModule;
using lotus::unittest::findBasicBlockByName;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModuleChecked;

TEST(SCCPTest, LatticeMeetMatchesThreePointSemantics) {
  llvm::LLVMContext context;
  auto *five = llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context),
                                                                    5));
  auto *three = llvm::cast<llvm::ConstantInt>(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 3));

  EXPECT_EQ(SccpValue::getTop().meet(SccpValue::getConstant(five)),
            SccpValue::getConstant(five));
  EXPECT_EQ(SccpValue::getConstant(five).meet(SccpValue::getConstant(five)),
            SccpValue::getConstant(five));
  EXPECT_EQ(SccpValue::getConstant(five).meet(SccpValue::getConstant(three)),
            SccpValue::getBottom());
  EXPECT_EQ(SccpValue::getBottom().meet(SccpValue::getConstant(five)),
            SccpValue::getBottom());
}

TEST(SCCPTest, BuildsAndAnalyzesMinimalFunction) {
  llvm::LLVMContext context;
  auto module = std::make_unique<llvm::Module>("sccp-minimal", context);
  auto *function_type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  auto *function = llvm::Function::Create(function_type, llvm::GlobalValue::ExternalLinkage,
                                          "minimal", module.get());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();

  auto result = runSCCPOnFunction(*function);
  EXPECT_TRUE(result.constants.empty());
  EXPECT_TRUE(result.dead_blocks.empty());
}

TEST(SCCPTest, DetectsDeadElseBranchFromConstantComparison) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @branchy() {
    entry:
      %cond = icmp eq i32 5, 5
      br i1 %cond, label %then, label %else
    then:
      ret i32 1
    else:
      ret i32 0
    }
  )");
  auto *function = module->getFunction("branchy");
  ASSERT_NE(function, nullptr);

  auto result = runSCCPOnFunction(*function);
  auto *cond = findInstructionByName(function, "cond");
  auto *else_block = findBasicBlockByName(*function, "else");
  ASSERT_NE(cond, nullptr);
  ASSERT_NE(else_block, nullptr);

  auto it = result.constants.find(cond);
  ASSERT_NE(it, result.constants.end());
  EXPECT_TRUE(it->second->isOne());
  EXPECT_TRUE(result.dead_blocks.contains(else_block));
}

TEST(SCCPTest, PropagatesReadOnlyGlobalLoadsAtModuleScope) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @g = global i32 5

    define i32 @load_global() {
    entry:
      %value = load i32, i32* @g
      ret i32 %value
    }
  )");

  auto result = runSCCPOnModule(*module);
  auto *function = module->getFunction("load_global");
  auto *value = findInstructionByName(function, "value");
  ASSERT_NE(function, nullptr);
  ASSERT_NE(value, nullptr);

  auto function_it = result.function_results.find(function);
  ASSERT_NE(function_it, result.function_results.end());

  auto constant_it = function_it->second.constants.find(value);
  ASSERT_NE(constant_it, function_it->second.constants.end());
  EXPECT_EQ(constant_it->second->getSExtValue(), 5);
}

TEST(SCCPTest, ResolvesPhiAndTruncationConstants) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i8 @phi_and_trunc(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %phi = phi i32 [ 7, %then ], [ 7, %else ]
      %tr = trunc i32 256 to i8
      %sum = add i8 %tr, 1
      ret i8 %sum
    }
  )");
  auto *function = module->getFunction("phi_and_trunc");
  ASSERT_NE(function, nullptr);

  auto result = runSCCPOnFunction(*function);
  auto *phi = findInstructionByName(function, "phi");
  auto *tr = findInstructionByName(function, "tr");
  auto *sum = findInstructionByName(function, "sum");
  ASSERT_NE(phi, nullptr);
  ASSERT_NE(tr, nullptr);
  ASSERT_NE(sum, nullptr);

  ASSERT_NE(result.constants.find(phi), result.constants.end());
  ASSERT_NE(result.constants.find(tr), result.constants.end());
  ASSERT_NE(result.constants.find(sum), result.constants.end());
  EXPECT_EQ(result.constants.find(phi)->second->getSExtValue(), 7);
  EXPECT_EQ(result.constants.find(tr)->second->getZExtValue(), 0U);
  EXPECT_EQ(result.constants.find(sum)->second->getZExtValue(), 1U);
}

TEST(SCCPTest, ResolvesSwitchTargetsWithoutFoldingCalls) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @callee() {
    entry:
      ret i32 42
    }

    define i32 @caller() {
    entry:
      %call = call i32 @callee()
      switch i32 2, label %default [
        i32 1, label %one
        i32 2, label %two
      ]
    one:
      ret i32 1
    two:
      ret i32 %call
    default:
      ret i32 0
    }
  )");
  auto *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  auto result = runSCCPOnFunction(*caller);
  auto *call = findInstructionByName(caller, "call");
  auto *one = findBasicBlockByName(*caller, "one");
  auto *default_block = findBasicBlockByName(*caller, "default");
  ASSERT_NE(call, nullptr);
  ASSERT_NE(one, nullptr);
  ASSERT_NE(default_block, nullptr);

  EXPECT_EQ(result.constants.find(call), result.constants.end());
  EXPECT_TRUE(result.dead_blocks.contains(one));
  EXPECT_TRUE(result.dead_blocks.contains(default_block));
}

} // namespace
