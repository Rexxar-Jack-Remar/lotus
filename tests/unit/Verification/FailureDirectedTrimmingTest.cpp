#include <gtest/gtest.h>

#include "Verification/FailureDirectedTrimming/FailureDirectedTrimming.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"

using namespace llvm;

namespace {

static std::unique_ptr<Module> parseModule(LLVMContext &Ctx, const char *IR) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(IR, Err, Ctx);
  if (!M) {
    std::string Msg;
    raw_string_ostream OS(Msg);
    Err.print("FailureDirectedTrimmingTest", OS);
    ADD_FAILURE() << OS.str();
  }
  return M;
}

static unsigned countCallsTo(const Function &F, StringRef CalleeName) {
  unsigned Count = 0;
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      const Function *Callee = CI->getCalledFunction();
      if (Callee && Callee->getName() == CalleeName)
        ++Count;
    }
  }
  return Count;
}

static bool moduleHasFunctionWithPrefix(const Module &M, StringRef Prefix) {
  for (const Function &F : M) {
    if (F.getName().startswith(Prefix))
      return true;
  }
  return false;
}

} // namespace

TEST(FailureDirectedTrimmingPassTest, CreatesSafeClonesAndWrapsCalls) {
  LLVMContext Ctx;
  auto M = parseModule(Ctx, R"IR(
; ModuleID = 'fdtrim-test'
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx14.0.0"

declare void @__VERIFIER_error()

define i32 @foo(i32 %x) {
entry:
  %cmp = icmp slt i32 %x, 0
  br i1 %cmp, label %err, label %ok
err:
  call void @__VERIFIER_error()
  unreachable
ok:
  ret i32 %x
}

define i32 @bar(i32 %x) {
entry:
  %y = call i32 @foo(i32 %x)
  %cmp2 = icmp eq i32 %y, 0
  br i1 %cmp2, label %err, label %ok
err:
  call void @__VERIFIER_error()
  unreachable
ok:
  ret i32 %y
}
)IR");
  ASSERT_TRUE(M);
  ASSERT_FALSE(verifyModule(*M, &errs()));

  ModuleAnalysisManager MAM;
  FailureDirectedTrimmingPass Pass;
  Pass.run(*M, MAM);

  Function *FooSafe = M->getFunction("foo.fdtrim.safe");
  Function *BarSafe = M->getFunction("bar.fdtrim.safe");
  ASSERT_NE(FooSafe, nullptr);
  ASSERT_NE(BarSafe, nullptr);

  Function *Bar = M->getFunction("bar");
  ASSERT_NE(Bar, nullptr);

  // The original bar should contain at least one call to the safe foo version.
  EXPECT_GE(countCallsTo(*Bar, "foo.fdtrim.safe"), 1u);

  // The module should contain verifier.assume (used by the transformation and
  // by trimming instrumentation).
  EXPECT_NE(M->getFunction("verifier.assume"), nullptr);
}

TEST(FailureDirectedTrimmingPassTest, SafeCloneConvertsCrabAssertToAssume) {
  LLVMContext Ctx;
  auto M = parseModule(Ctx, R"IR(
; ModuleID = 'fdtrim-assert-test'
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx14.0.0"

declare void @__CRAB_assert(i32)

define void @foo(i32 %x) {
entry:
  call void @__CRAB_assert(i32 %x)
  ret void
}
)IR");
  ASSERT_TRUE(M);
  ASSERT_FALSE(verifyModule(*M, &errs()));

  ModuleAnalysisManager MAM;
  FailureDirectedTrimmingPass Pass;
  Pass.run(*M, MAM);

  Function *FooSafe = M->getFunction("foo.fdtrim.safe");
  ASSERT_NE(FooSafe, nullptr);

  EXPECT_EQ(countCallsTo(*FooSafe, "__CRAB_assert"), 0u);
  EXPECT_GE(countCallsTo(*FooSafe, "verifier.assume"), 1u);
}

TEST(FailureDirectedTrimmingPassTest, DefaultDerefCodegenUsesUF) {
  LLVMContext Ctx;
  auto M = parseModule(Ctx, R"IR(
; ModuleID = 'fdtrim-deref-test'
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx14.0.0"

declare void @__VERIFIER_error()

define void @foo(i32* %p) {
entry:
  br label %loop

loop:
  %x = load i32, i32* %p
  %cmp = icmp eq i32 %x, 0
  br i1 %cmp, label %err, label %loop

err:
  call void @__VERIFIER_error()
  unreachable
}
)IR");
  ASSERT_TRUE(M);
  ASSERT_FALSE(verifyModule(*M, &errs()));

  ModuleAnalysisManager MAM;
  FailureDirectedTrimmingPass Pass;
  Pass.run(*M, MAM);

  // The default deref mode is "uf", so inserted assumptions should reference
  // verifier.drf.trim.* helper functions rather than emitting new loads.
  EXPECT_TRUE(moduleHasFunctionWithPrefix(*M, "verifier.drf.trim."));
  EXPECT_NE(M->getFunction("verifier.assume"), nullptr);
}
