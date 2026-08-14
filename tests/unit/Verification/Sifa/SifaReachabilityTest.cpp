#include "Verification/Sifa/Sifa.h"

#include "TestUtils/LLVMHelpers.h"

#include "gtest/gtest.h"

using namespace llvm;
using namespace lotus::unittest;

template <typename FunctionT>
static auto getBlockByName(FunctionT &F, const char *name) {
  return findBlock(F, name);
}

TEST(SifaReachability, ReachableAndUnreachableBlocks) {
  const char *ir = R"IR(
    define i32 @f(i32 %n) {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %inc = add i32 %i, 1
      br label %loop

    exit:
      ret i32 0

    unreach:
      ret i32 1
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaReachability");

  Function *F = getFunctionChecked(*M, "f");

  const BasicBlock *body = getBlockChecked(*F, "body");
  const BasicBlock *exit = getBlockChecked(*F, "exit");
  const BasicBlock *unreach = getBlockChecked(*F, "unreach");

  EXPECT_TRUE(lotus::sifa::isReachable(*F, *body));
  EXPECT_TRUE(lotus::sifa::isReachable(*F, *exit));
  EXPECT_FALSE(lotus::sifa::isReachable(*F, *unreach));
}

TEST(SifaReachability, SingleBlockReachable) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaReachability");

  Function *F = getFunctionChecked(*M, "f");
  const BasicBlock *entry = &F->getEntryBlock();

  EXPECT_TRUE(lotus::sifa::isReachable(*F, *entry));
}

TEST(SifaReachability, ConditionalBranchBothTargetsReachable) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %c = icmp eq i32 %x, 0
      br i1 %c, label %then, label %else

    then:
      ret i32 1

    else:
      ret i32 2
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *thenBB = getBlockByName(*F, "then");
  const llvm::BasicBlock *elseBB = getBlockByName(*F, "else");
  ASSERT_NE(thenBB, nullptr);
  ASSERT_NE(elseBB, nullptr);

  EXPECT_TRUE(lotus::sifa::isReachable(*F, *thenBB));
  EXPECT_TRUE(lotus::sifa::isReachable(*F, *elseBB));
}
