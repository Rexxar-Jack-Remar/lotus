#include "EliminationTestSupport.h"

TEST_F(APATest, LLVMReachabilitySkipsUnreachableBlock) {
  const char *Source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %live = add i32 1, 2
      br label %exit
    else:
      br label %exit
    dead:
      %deadv = add i32 40, 2
      br label %exit
    exit:
      %phi = phi i32 [ %live, %then ], [ 0, %else ]
      ret i32 %phi
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimReachable(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Live = findInstructionByName(F, "live");
  auto *Dead = findInstructionByName(F, "deadv");
  auto *Ret = findFirst<llvm::ReturnInst>(F);
  ASSERT_NE(Live, nullptr);
  ASSERT_NE(Dead, nullptr);
  ASSERT_NE(Ret, nullptr);

  ASSERT_NE(Result.tryIN(Live), nullptr);
  EXPECT_TRUE(*Result.tryIN(Live));
  ASSERT_NE(Result.tryIN(Ret), nullptr);
  EXPECT_TRUE(*Result.tryIN(Ret));
  EXPECT_EQ(Result.tryIN(Dead), nullptr);
}
TEST_F(APATest, LLVMConstantPropagationTracksFoldedValuesAtReturn) {
  const char *Source = R"(
    define i32 @test() {
    entry:
      %sum = add i32 1, 2
      %scaled = mul i32 %sum, 4
      ret i32 %scaled
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimConstantPropagation(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Scaled = findInstructionByName(F, "scaled");
  auto *Ret = findFirst<llvm::ReturnInst>(F);
  ASSERT_NE(Scaled, nullptr);
  ASSERT_NE(Ret, nullptr);

  auto *Facts = Result.tryIN(Ret);
  ASSERT_NE(Facts, nullptr);

  auto ScaledIt = Facts->find(Scaled);
  ASSERT_NE(ScaledIt, Facts->end());
}
TEST_F(APATest, LLVMLiveVariablesMergesFactsAcrossMultipleReturns) {
  const char *Source = R"(
    define i32 @test(i1 %cond, i32 %a, i32 %b) {
    entry:
      %sum = add i32 %a, %b
      br i1 %cond, label %ret_sum, label %ret_a
    ret_sum:
      ret i32 %sum
    ret_a:
      ret i32 %a
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimLiveVariables(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Sum = findInstructionByName(F, "sum");
  auto *A = F->getArg(1);
  auto *B = F->getArg(2);
  ASSERT_NE(Sum, nullptr);
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);

  auto BBIt = F->begin();
  ++BBIt; // ret_sum
  auto *RetSumInst = llvm::cast<llvm::ReturnInst>(BBIt->getTerminator());
  auto *RetAInst = llvm::cast<llvm::ReturnInst>(F->back().getTerminator());

  auto *RetSumFacts = Result.tryIN(RetSumInst);
  auto *RetAFacts = Result.tryIN(RetAInst);
  ASSERT_NE(RetSumFacts, nullptr);
  ASSERT_NE(RetAFacts, nullptr);
  EXPECT_NE(RetSumFacts->find(Sum), RetSumFacts->end());
  EXPECT_EQ(RetSumFacts->find(A), RetSumFacts->end());
  EXPECT_NE(RetAFacts->find(A), RetAFacts->end());
  EXPECT_EQ(RetAFacts->find(B), RetAFacts->end());
}
