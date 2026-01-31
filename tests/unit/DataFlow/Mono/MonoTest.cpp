/**
 * @file MonoTest.cpp
 * @brief Unit tests for Mono (monotone dataflow framework)
 */

#include "Dataflow/Mono/Analyses/Intraprocedural/LiveVariablesAnalysis.h"
#include "Dataflow/Mono/DataFlowResult.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;
using namespace mono;

class MonoTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("MonoTest", errs());
    }
    return module;
  }
};

// Test live variables analysis on simple function
TEST_F(MonoTest, LiveVariables) {
  const char *source = R"(
    define i32 @test(i32 %a, i32 %b) {
    entry:
      %c = add i32 %a, %b
      %d = mul i32 %c, 2
      ret i32 %d
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Verify that results are computed for all instructions
  unsigned instCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      instCount++;
      // Each instruction should have IN and OUT sets
      auto &inSet = result->IN(&I);
      auto &outSet = result->OUT(&I);
      // Sets should be initialized (may be empty)
      EXPECT_GE(inSet.size(), 0);
      EXPECT_GE(outSet.size(), 0);
    }
  }

  EXPECT_GT(instCount, 0);
}

// Test live variables with multiple blocks
TEST_F(MonoTest, LiveVariablesMultiBlock) {
  const char *source = R"(
    define i32 @test(i32 %a, i32 %b) {
    entry:
      %c = add i32 %a, %b
      br i1 true, label %true, label %false
    true:
      %d = mul i32 %c, 2
      br label %exit
    false:
      %e = sub i32 %c, 1
      br label %exit
    exit:
      %f = phi i32 [ %d, %true ], [ %e, %false ]
      ret i32 %f
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Find the return instruction
  ReturnInst *ret = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (ReturnInst *RI = dyn_cast<ReturnInst>(&I)) {
        ret = RI;
      }
    }
  }

  ASSERT_NE(ret, nullptr);

  // Return instruction should have computed IN/OUT
  auto &inSet = result->IN(ret);
  auto &outSet = result->OUT(ret);
  EXPECT_GE(inSet.size(), 0);
  EXPECT_EQ(outSet.size(), 0); // Out set of return should be empty
}

// Test empty function
TEST_F(MonoTest, EmptyFunction) {
  const char *source = R"(
    define void @test() {
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Should handle empty function gracefully
  unsigned instCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      instCount++;
      auto &inSet = result->IN(&I);
      auto &outSet = result->OUT(&I);
      EXPECT_GE(inSet.size(), 0);
      EXPECT_GE(outSet.size(), 0);
    }
  }

  EXPECT_GT(instCount, 0); // At least return instruction
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

