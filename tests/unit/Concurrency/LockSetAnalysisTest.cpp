/**
 * @file LockSetAnalysisTest.cpp
 * @brief Unit tests for Lock Set Analysis
 */

#include "Analysis/Concurrency/LockSet/LockSetAnalysis.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;

static const Instruction *findInstructionByName(const Function &func,
                                                StringRef name) {
  for (const auto &bb : func) {
    for (const auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

class LockSetAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("LockSetAnalysisTest", errs());
    }
    return module;
  }
};

TEST_F(LockSetAnalysisTest, BranchingMustAndMayLockSets) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock1 = global i8 0
    @lock2 = global i8 0

    define i32 @main() {
    entry:
      %l1 = call i32 @pthread_mutex_lock(i8* @lock1)
      %cond = icmp eq i32 0, 0
      br i1 %cond, label %then, label %else

    then:
      %l2 = call i32 @pthread_mutex_lock(i8* @lock2)
      %t = add i32 1, 2
      %u2 = call i32 @pthread_mutex_unlock(i8* @lock2)
      br label %merge

    else:
      %e = add i32 3, 4
      br label %merge

    merge:
      %m = add i32 5, 6
      %u1 = call i32 @pthread_mutex_unlock(i8* @lock1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  const Instruction *t = findInstructionByName(*main_func, "t");
  const Instruction *e = findInstructionByName(*main_func, "e");
  const Instruction *m = findInstructionByName(*main_func, "m");
  ASSERT_NE(t, nullptr);
  ASSERT_NE(e, nullptr);
  ASSERT_NE(m, nullptr);

  const GlobalVariable *lock1 = module->getNamedGlobal("lock1");
  const GlobalVariable *lock2 = module->getNamedGlobal("lock2");
  ASSERT_NE(lock1, nullptr);
  ASSERT_NE(lock2, nullptr);

  EXPECT_TRUE(lsa.mustHoldLock(t, lock1));
  EXPECT_TRUE(lsa.mustHoldLock(t, lock2));
  EXPECT_TRUE(lsa.mustHoldLock(e, lock1));
  EXPECT_FALSE(lsa.mustHoldLock(e, lock2));
  EXPECT_TRUE(lsa.mustHoldLock(m, lock1));
  EXPECT_TRUE(lsa.mayHoldLock(m, lock2));
  EXPECT_EQ(lsa.getLockNestingDepth(t), 2u);
}

TEST_F(LockSetAnalysisTest, TryLockIsMayOnly) {
  const char *source = R"(
    declare i32 @pthread_mutex_trylock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define i32 @main() {
    entry:
      %try = call i32 @pthread_mutex_trylock(i8* @lock)
      %after = add i32 1, 2
      %u = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  const Instruction *after = findInstructionByName(*main_func, "after");
  ASSERT_NE(after, nullptr);

  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);

  EXPECT_FALSE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest, DetectLockOrderInversion) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lockA = global i8 0
    @lockB = global i8 0

    define void @f1() {
    entry:
      %a1 = call i32 @pthread_mutex_lock(i8* @lockA)
      %b1 = call i32 @pthread_mutex_lock(i8* @lockB)
      %bu1 = call i32 @pthread_mutex_unlock(i8* @lockB)
      %au1 = call i32 @pthread_mutex_unlock(i8* @lockA)
      ret void
    }

    define void @f2() {
    entry:
      %b2 = call i32 @pthread_mutex_lock(i8* @lockB)
      %a2 = call i32 @pthread_mutex_lock(i8* @lockA)
      %au2 = call i32 @pthread_mutex_unlock(i8* @lockA)
      %bu2 = call i32 @pthread_mutex_unlock(i8* @lockB)
      ret void
    }

    define i32 @main() {
      call void @f1()
      call void @f2()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const GlobalVariable *lockA = module->getNamedGlobal("lockA");
  const GlobalVariable *lockB = module->getNamedGlobal("lockB");
  ASSERT_NE(lockA, nullptr);
  ASSERT_NE(lockB, nullptr);

  EXPECT_FALSE(lsa.areLocksOrderedConsistently(lockA, lockB));
  EXPECT_GT(lsa.detectLockOrderInversions().size(), 0u);
}

TEST_F(LockSetAnalysisTest, InvokeAppliesInterproceduralSummaries) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @__gxx_personality_v0(...)

    @lock = global i8 0

    define void @lock_helper(i8* %m) {
    entry:
      %l = call i32 @pthread_mutex_lock(i8* %m)
      ret void
    }

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      invoke void @lock_helper(i8* @lock) to label %cont unwind label %lpad

    cont:
      %after = add i32 1, 2
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *after = findInstructionByName(*main_func, "after");
  ASSERT_NE(after, nullptr);

  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest, IndirectInvokeDoesNotInheritOtherCallersCallees) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @__gxx_personality_v0(...)

    @lock = global i8 0

    define void @lock_helper(i8* %m) {
    entry:
      %l = call i32 @pthread_mutex_lock(i8* %m)
      ret void
    }

    define void @noop(i8* %m) {
    entry:
      ret void
    }

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %fn = select i1 true, void (i8*)* @noop, void (i8*)* @noop
      invoke void %fn(i8* @lock) to label %cont unwind label %lpad

    cont:
      %after = add i32 1, 2
      call void @lock_helper(i8* @lock)
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *after = findInstructionByName(*main_func, "after");
  ASSERT_NE(after, nullptr);

  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);

  EXPECT_FALSE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
