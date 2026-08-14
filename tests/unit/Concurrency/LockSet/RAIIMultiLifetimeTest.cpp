#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;
using namespace lotus::unittest;

class RAIIMultiLifetimeTest : public LlvmModuleTest {
protected:
  using LlvmModuleTest::parseModule;
};

TEST_F(RAIIMultiLifetimeTest, LoopRAII) {
  const char *source = R"(
    declare void @fake_lock_guard_C1E(i8*, i8*)
    declare void @fake_lock_guard_D1Ev(i8*)

    @m = global i8 0

    define void @test_loop(i32 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
      %guard = alloca i8
      call void @fake_lock_guard_C1E(i8* %guard, i8* @m)
      ; critical section
      %val = load i8, i8* @m
      call void @fake_lock_guard_D1Ev(i8* %guard)
      %i.next = add i32 %i, 1
      %cmp = icmp slt i32 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *func = module->getFunction("test_loop");
  const Instruction *loadM = nullptr;
  for (auto &BB : *func) {
    for (auto &I : BB) {
      if (isa<LoadInst>(I)) {
        loadM = &I;
        break;
      }
    }
  }
  ASSERT_NE(loadM, nullptr);

  EXPECT_TRUE(lsa.mustHoldLock(loadM, module->getNamedGlobal("m")));
}
