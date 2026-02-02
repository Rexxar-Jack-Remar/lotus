#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/SymbolicAbstraction/Core/AbstractValue.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <memory>

namespace {

static llvm::BasicBlock *getBlockByName(llvm::Function &F, const char *name) {
  for (llvm::BasicBlock &BB : F) {
    if (BB.getName() == name) {
      return &BB;
    }
  }
  return nullptr;
}

TEST(SifaSymAbs, SmokeIntervalsOctagonAndCalls) {
  const char *ir = R"IR(
    define i32 @g(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }

    define i32 @f(i32 %n) {
    entry:
      %c = call i32 @g(i32 %n)
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, %c
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

  llvm::BasicBlock *body = getBlockByName(*F, "body");
  llvm::BasicBlock *exit = getBlockByName(*F, "exit");
  llvm::BasicBlock *unreach = getBlockByName(*F, "unreach");
  ASSERT_NE(body, nullptr);
  ASSERT_NE(exit, nullptr);
  ASSERT_NE(unreach, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval, Octagon";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  EXPECT_TRUE(lotus::sifa::isReachableSymAbs(*M, *F, *body, opt));
  EXPECT_TRUE(lotus::sifa::isReachableSymAbs(*M, *F, *exit, opt));
  EXPECT_FALSE(lotus::sifa::isReachableSymAbs(*M, *F, *unreach, opt));

  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

TEST(SifaSymAbs, SingleBlockFunctionReturnStateNotBottom) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

TEST(SifaSymAbs, AnalyzeSymAbsToSpecificBlock) {
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

  llvm::BasicBlock *body = getBlockByName(*F, "body");
  llvm::BasicBlock *exit = getBlockByName(*F, "exit");
  llvm::BasicBlock *unreach = getBlockByName(*F, "unreach");
  ASSERT_NE(body, nullptr);
  ASSERT_NE(exit, nullptr);
  ASSERT_NE(unreach, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval, Octagon";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  auto stateBody = lotus::sifa::analyzeSymAbsTo(*M, *F, *body, opt);
  auto stateExit = lotus::sifa::analyzeSymAbsTo(*M, *F, *exit, opt);
  auto stateUnreach = lotus::sifa::analyzeSymAbsTo(*M, *F, *unreach, opt);

  EXPECT_NE(stateBody, nullptr);
  EXPECT_NE(stateExit, nullptr);
  EXPECT_FALSE(stateBody->isBottom());
  EXPECT_FALSE(stateExit->isBottom());
  EXPECT_TRUE(stateUnreach == nullptr || stateUnreach->isBottom());
}

TEST(SifaSymAbs, IntervalOnlyDomain) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  EXPECT_TRUE(lotus::sifa::isReachableSymAbs(*M, *F, F->getEntryBlock(), opt));
  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

} // namespace
