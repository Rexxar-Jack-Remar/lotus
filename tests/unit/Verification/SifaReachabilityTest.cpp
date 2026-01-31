#include "Verification/Sifa/Sifa.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <memory>

namespace {

static const llvm::BasicBlock *getBlockByName(const llvm::Function &F, const char *name) {
  for (const llvm::BasicBlock &BB : F) {
    if (BB.getName() == name) {
      return &BB;
    }
  }
  return nullptr;
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

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *body = getBlockByName(*F, "body");
  const llvm::BasicBlock *exit = getBlockByName(*F, "exit");
  const llvm::BasicBlock *unreach = getBlockByName(*F, "unreach");
  ASSERT_NE(body, nullptr);
  ASSERT_NE(exit, nullptr);
  ASSERT_NE(unreach, nullptr);

  EXPECT_TRUE(lotus::sifa::isReachable(*F, *body));
  EXPECT_TRUE(lotus::sifa::isReachable(*F, *exit));
  EXPECT_FALSE(lotus::sifa::isReachable(*F, *unreach));
}

} // namespace
