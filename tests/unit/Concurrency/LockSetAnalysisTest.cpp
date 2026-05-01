#include "LockSetAnalysisTestSupport.h"

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

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest, ConditionalHelperUnlockClearsCallerMustButNotMay) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @helper(i1 %cond) {
    entry:
      br i1 %cond, label %unlock, label %done

    unlock:
      call i32 @pthread_mutex_unlock(i8* @lock)
      br label %done

    done:
      ret void
    }

    define i32 @main(i1 %cond) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      call void @helper(i1 %cond)
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest, GuaranteedHelperUnlockClearsCallerLocksets) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      call void @helper()
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_FALSE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest, CountingSemaphoreDoesNotCreateMutualExclusion) {
  const char *source = R"(
    declare i32 @sem_wait(i8*)

    @sem = global i8 0

    define void @worker1() {
    entry:
      call i32 @sem_wait(i8* @sem)
      %store1 = add i32 1, 2
      ret void
    }

    define void @worker2() {
    entry:
      call i32 @sem_wait(i8* @sem)
      %store2 = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *store1 =
      findInstructionByName(*module->getFunction("worker1"), "store1");
  const Instruction *store2 =
      findInstructionByName(*module->getFunction("worker2"), "store2");
  const GlobalVariable *sem = module->getNamedGlobal("sem");
  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  ASSERT_NE(sem, nullptr);

  EXPECT_FALSE(lsa.mayHoldLock(store1, sem));
  EXPECT_FALSE(lsa.mayHoldLock(store2, sem));
  EXPECT_FALSE(lsa.mayHoldCommonLock(store1, store2));
  EXPECT_FALSE(lsa.mustHoldCommonLock(store1, store2));
}
TEST_F(LockSetAnalysisTest, CountingSemaphoreDoesNotPopulateLocksets) {
  const char *source = R"(
    declare i32 @sem_wait(i8*)

    @sem = global i8 0

    define i32 @main() {
    entry:
      call i32 @sem_wait(i8* @sem)
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *sem = module->getNamedGlobal("sem");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(sem, nullptr);

  EXPECT_FALSE(lsa.mayHoldLock(after, sem));
  EXPECT_FALSE(lsa.mustHoldLock(after, sem));
}
TEST_F(LockSetAnalysisTest, BinarySemaphoreTraitOptInPreservesExclusion) {
  const char *source = R"(
    declare i32 @binary_sem_wait(i8*)

    @sem = global i8 0

    define void @worker1() {
    entry:
      call i32 @binary_sem_wait(i8* @sem)
      %store1 = add i32 1, 2
      ret void
    }

    define void @worker2() {
    entry:
      call i32 @binary_sem_wait(i8* @sem)
      %store2 = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *store1 =
      findInstructionByName(*module->getFunction("worker1"), "store1");
  const Instruction *store2 =
      findInstructionByName(*module->getFunction("worker2"), "store2");
  const GlobalVariable *sem = module->getNamedGlobal("sem");
  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  ASSERT_NE(sem, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(store1, sem));
  EXPECT_TRUE(lsa.mayHoldLock(store2, sem));
  EXPECT_TRUE(lsa.mayHoldCommonLock(store1, store2));
  EXPECT_TRUE(lsa.mustHoldCommonLock(store1, store2));
}
TEST_F(LockSetAnalysisTest,
       SemaphorePolicyRemainsConsistentAcrossInterproceduralSummaries) {
  const char *source = R"(
    declare i32 @sem_wait(i8*)
    declare i32 @binary_sem_wait(i8*)

    @sem = global i8 0
    @binary = global i8 0

    define void @counting_helper() {
    entry:
      call i32 @sem_wait(i8* @sem)
      ret void
    }

    define void @binary_helper() {
    entry:
      call i32 @binary_sem_wait(i8* @binary)
      ret void
    }

    define i32 @main() {
    entry:
      call void @counting_helper()
      %after_counting = add i32 1, 2
      call void @binary_helper()
      %after_binary = add i32 3, 4
      ret i32 %after_binary
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *after_counting =
      findInstructionByName(*main_func, "after_counting");
  const Instruction *after_binary =
      findInstructionByName(*main_func, "after_binary");
  const GlobalVariable *sem = module->getNamedGlobal("sem");
  const GlobalVariable *binary = module->getNamedGlobal("binary");
  ASSERT_NE(after_counting, nullptr);
  ASSERT_NE(after_binary, nullptr);
  ASSERT_NE(sem, nullptr);
  ASSERT_NE(binary, nullptr);

  EXPECT_FALSE(lsa.mayHoldLock(after_counting, sem));
  EXPECT_FALSE(lsa.mustHoldLock(after_counting, sem));
  EXPECT_FALSE(lsa.mayHoldLock(after_binary, binary));
  EXPECT_FALSE(lsa.mustHoldLock(after_binary, binary));
}
TEST_F(LockSetAnalysisTest, ScopedLockTracksAllUnderlyingMutexes) {
  const char *source = R"(
    declare void @fake_scoped_lock_C1E(i8*, i8*, i8*)

    @lock1 = global i8 0
    @lock2 = global i8 0

    define i32 @main() {
    entry:
      %sl = alloca i8
      call void @fake_scoped_lock_C1E(i8* %sl, i8* @lock1, i8* @lock2)
      %after = add i32 1, 2
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

  const GlobalVariable *lock1 = module->getNamedGlobal("lock1");
  const GlobalVariable *lock2 = module->getNamedGlobal("lock2");
  ASSERT_NE(lock1, nullptr);
  ASSERT_NE(lock2, nullptr);

  EXPECT_TRUE(lsa.mustHoldLock(after, lock1));
  EXPECT_TRUE(lsa.mustHoldLock(after, lock2));
}
TEST_F(LockSetAnalysisTest, ImpreciseRaiiScopeDoesNotLeakMustLockPastBoundary) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)

    @lock = global i8 0

    define i32 @main() {
    entry:
      %ul = alloca i8
      br label %scope

    scope:
      call void @fake_unique_lock_C1E(i8* %ul, i8* @lock)
      br label %after

    after:
      %after_scope = add i32 1, 2
      ret i32 %after_scope
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after_scope =
      findInstructionByName(*module->getFunction("main"), "after_scope");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after_scope, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_FALSE(lsa.mustHoldLock(after_scope, lock));
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
TEST_F(LockSetAnalysisTest, UniqueLockManualLockUnlockUsesUnderlyingMutex) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @fake_unique_lock_unlockEv(i8*)
    declare void @fake_unique_lock_lockEv(i8*)

    @lock = global i8 0

    define i32 @main() {
    entry:
      %ul = alloca i8
      call void @fake_unique_lock_C1E(i8* %ul, i8* @lock)
      call void @fake_unique_lock_unlockEv(i8* %ul)
      call void @fake_unique_lock_lockEv(i8* %ul)
      %after = add i32 1, 2
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

  EXPECT_TRUE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest, DoubleRawAcquireMarksLockReentrant) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      call i32 @pthread_mutex_lock(i8* @lock)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);
  EXPECT_TRUE(lsa.isReentrantLock(lock));
}
TEST_F(LockSetAnalysisTest, UniqueLockManualRelockMarksLockReentrant) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @fake_unique_lock_lockEv(i8*)

    @lock = global i8 0

    define i32 @main() {
    entry:
      %ul = alloca i8
      call void @fake_unique_lock_C1E(i8* %ul, i8* @lock)
      call void @fake_unique_lock_lockEv(i8* %ul)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);
  EXPECT_TRUE(lsa.isReentrantLock(lock));
}
TEST_F(LockSetAnalysisTest, ConditionalLockDoesNotBecomeMustCommonLock) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0
    @flag = external global i1

    define void @worker1() {
    entry:
      %cond = load i1, i1* @flag
      br i1 %cond, label %locked, label %merge

    locked:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    merge:
      %store1 = add i32 1, 2
      ret void
    }

    define void @worker2() {
    entry:
      %cond = load i1, i1* @flag
      br i1 %cond, label %locked, label %merge

    locked:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    merge:
      %store2 = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *store1 =
      findInstructionByName(*module->getFunction("worker1"), "store1");
  const Instruction *store2 =
      findInstructionByName(*module->getFunction("worker2"), "store2");
  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);

  EXPECT_TRUE(lsa.mayHoldCommonLock(store1, store2));
  EXPECT_FALSE(lsa.mustHoldCommonLock(store1, store2));
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

  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest,
       PartiallyUnresolvedIndirectCallKeepsMayButDropsMustLockState) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare void @external_effect(i8*)

    @lock = global i8 0

    define void @lock_helper(i8* %m) {
    entry:
      call i32 @pthread_mutex_lock(i8* %m)
      ret void
    }

    define void @main(i1 %cond) {
    entry:
      %fn = select i1 %cond, void (i8*)* @lock_helper,
                         void (i8*)* @external_effect
      call void %fn(i8* @lock)
      %after = add i32 1, 2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest,
       ResolvedIndirectAcquireUsesMayUnionAndMustIntersection) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define void @lock_helper(i8* %m) {
    entry:
      call i32 @pthread_mutex_lock(i8* %m)
      ret void
    }

    define void @noop(i8* %m) {
    entry:
      ret void
    }

    define void @main(i1 %cond) {
    entry:
      %fn = select i1 %cond, void (i8*)* @lock_helper, void (i8*)* @noop
      call void %fn(i8* @lock)
      %after = add i32 1, 2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest,
       ResolvedIndirectReleaseUsesMayUnionAndMustIntersection) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @unlock_helper(i8* %m) {
    entry:
      call i32 @pthread_mutex_unlock(i8* %m)
      ret void
    }

    define void @noop(i8* %m) {
    entry:
      ret void
    }

    define void @main(i1 %cond) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %fn = select i1 %cond, void (i8*)* @unlock_helper, void (i8*)* @noop
      call void %fn(i8* @lock)
      %after = add i32 1, 2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest, UniqueLockDeferDoesNotAcquireAtConstruction) {
  const char *source = R"(
    declare void @fake_unique_lock_defer_lock_C1E(i8*, i8*, i8*)

    @lock = global i8 0
    @tag = global i8 0

    define i32 @main() {
    entry:
      %ul = alloca i8
      call void @fake_unique_lock_defer_lock_C1E(i8* %ul, i8* @lock, i8* @tag)
      %after = add i32 1, 2
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest, UniqueLockTryToLockIsMayOnly) {
  const char *source = R"(
    declare void @fake_unique_lock_try_to_lock_C1E(i8*, i8*, i8*)

    @lock = global i8 0
    @tag = global i8 0

    define i32 @main() {
    entry:
      %ul = alloca i8
      call void @fake_unique_lock_try_to_lock_C1E(i8* %ul, i8* @lock, i8* @tag)
      %after = add i32 1, 2
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);
  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest, SharedLockCountsAsReadLockOnly) {
  const char *source = R"(
    declare void @fake_shared_lock_C1E(i8*, i8*)

    @lock = global i8 0

    define i32 @main() {
    entry:
      %sl = alloca i8
      call void @fake_shared_lock_C1E(i8* %sl, i8* @lock)
      %after = add i32 1, 2
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);
  EXPECT_TRUE(lsa.getMayReadLockSetAt(after).count(lock) > 0);
  EXPECT_TRUE(lsa.getMayWriteLockSetAt(after).count(lock) == 0);
}
