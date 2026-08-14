#include "LockSetAnalysisTestSupport.h"

TEST_F(LockSetAnalysisTest, DirectBooleanUniqueLockTryRefinesSuccessEdge) {
  const char *source = R"(
    declare void @fake_unique_lock_defer_lock_C1E(i8*, i8*, i8*)
    declare i1 @_ZNSt11unique_lockISt5mutexE8try_lockEv(i8*)
    @lock = global i8 0
    @tag = global i8 0
    define void @test() {
    entry:
      %u = alloca i8
      call void @fake_unique_lock_defer_lock_C1E(i8* %u, i8* @lock, i8* @tag)
      %ok = call i1 @_ZNSt11unique_lockISt5mutexE8try_lockEv(i8* %u)
      br i1 %ok, label %success, label %failure
    success:
      %held = add i32 1, 2
      ret void
    failure:
      %not_held = add i32 3, 4
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Function *test = module->getFunction("test");
  const Value *lock = module->getNamedGlobal("lock");
  const Value *tag = module->getNamedGlobal("tag");
  const Instruction *held = findInstructionByName(*test, "held");
  const Instruction *not_held = findInstructionByName(*test, "not_held");
  EXPECT_TRUE(lsa.mustHoldLock(held, lock));
  EXPECT_FALSE(lsa.mayHoldLock(not_held, lock));
  EXPECT_FALSE(lsa.mayHoldLock(held, tag));
}

TEST_F(LockSetAnalysisTest, RwTryLockRefinesCorrectEdgeAndMode) {
  const char *source = R"(
    declare i32 @pthread_rwlock_tryrdlock(i8*)
    declare i32 @pthread_rwlock_trywrlock(i8*)

    @read_lock = global i8 0
    @write_lock = global i8 0

    define void @test(i1 %which) {
    entry:
      %read_result = call i32 @pthread_rwlock_tryrdlock(i8* @read_lock)
      %read_failed = icmp ne i32 %read_result, 0
      br i1 %read_failed, label %read_fail, label %read_success

    read_success:
      %read_ok = add i32 1, 2
      br label %write_try

    read_fail:
      %read_bad = add i32 3, 4
      br label %write_try

    write_try:
      %write_result = call i32 @pthread_rwlock_trywrlock(i8* @write_lock)
      %write_ok_cmp = icmp eq i32 %write_result, 0
      br i1 %write_ok_cmp, label %write_success, label %write_fail

    write_success:
      %write_ok = add i32 5, 6
      ret void

    write_fail:
      %write_bad = add i32 7, 8
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *test = module->getFunction("test");
  const Instruction *read_ok = findInstructionByName(*test, "read_ok");
  const Instruction *read_bad = findInstructionByName(*test, "read_bad");
  const Instruction *write_ok = findInstructionByName(*test, "write_ok");
  const Instruction *write_bad = findInstructionByName(*test, "write_bad");
  const GlobalVariable *read_lock = module->getNamedGlobal("read_lock");
  const GlobalVariable *write_lock = module->getNamedGlobal("write_lock");
  ASSERT_NE(read_ok, nullptr);
  ASSERT_NE(read_bad, nullptr);
  ASSERT_NE(write_ok, nullptr);
  ASSERT_NE(write_bad, nullptr);

  EXPECT_EQ(lsa.getMustReadLockSetAt(read_ok).count(read_lock), 1u);
  EXPECT_EQ(lsa.getMustWriteLockSetAt(read_ok).count(read_lock), 0u);
  EXPECT_EQ(lsa.getMayLockSetAt(read_bad).count(read_lock), 0u);
  EXPECT_EQ(lsa.getMustWriteLockSetAt(write_ok).count(write_lock), 1u);
  EXPECT_EQ(lsa.getMayLockSetAt(write_bad).count(write_lock), 0u);
}

TEST_F(LockSetAnalysisTest, TryLockSuccessFactIsEdgeSpecific) {
  const char *source = R"(
    declare i32 @pthread_mutex_trylock(i8*)

    @lock = global i8 0

    define void @test(i1 %bypass) {
    entry:
      br i1 %bypass, label %try, label %success

    try:
      %result = call i32 @pthread_mutex_trylock(i8* @lock)
      %ok = icmp eq i32 %result, 0
      br i1 %ok, label %success, label %failure

    success:
      %at_success = add i32 1, 2
      ret void

    failure:
      %at_failure = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *test = module->getFunction("test");
  const Instruction *success = findInstructionByName(*test, "at_success");
  const Instruction *failure = findInstructionByName(*test, "at_failure");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(success, nullptr);
  ASSERT_NE(failure, nullptr);
  EXPECT_EQ(lsa.getMustLockSetAt(success).count(lock), 0u);
  EXPECT_EQ(lsa.getMayLockSetAt(success).count(lock), 1u);
  EXPECT_EQ(lsa.getMayLockSetAt(failure).count(lock), 0u);
}

TEST_F(LockSetAnalysisTest, BooleanTryLockPredicatesRefineAllCommonForms) {
  const char *source = R"(
    declare i1 @_ZNSt5mutex8try_lockEv(i8*)
    @direct_lock = global i8 0
    @negated_lock = global i8 0
    @one_lock = global i8 0

    define void @direct() {
    entry:
      %ok = call i1 @_ZNSt5mutex8try_lockEv(i8* @direct_lock)
      br i1 %ok, label %success, label %failure
    success:
      %direct_success = add i32 1, 2
      ret void
    failure:
      %direct_failure = add i32 3, 4
      ret void
    }

    define void @negated() {
    entry:
      %ok = call i1 @_ZNSt5mutex8try_lockEv(i8* @negated_lock)
      %failed = xor i1 %ok, true
      br i1 %failed, label %failure, label %success
    success:
      %negated_success = add i32 5, 6
      ret void
    failure:
      %negated_failure = add i32 7, 8
      ret void
    }

    define void @compared_with_one() {
    entry:
      %ok = call i1 @_ZNSt5mutex8try_lockEv(i8* @one_lock)
      %cmp = icmp eq i1 %ok, true
      br i1 %cmp, label %success, label %failure
    success:
      %one_success = add i32 9, 10
      ret void
    failure:
      %one_failure = add i32 11, 12
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  struct Expected {
    const char *function;
    const char *success;
    const char *failure;
    const char *lock;
  };
  for (const Expected expected : {
           Expected{"direct", "direct_success", "direct_failure",
                    "direct_lock"},
           Expected{"negated", "negated_success", "negated_failure",
                    "negated_lock"},
           Expected{"compared_with_one", "one_success", "one_failure",
                    "one_lock"}}) {
    const Function *function = module->getFunction(expected.function);
    const Instruction *success =
        findInstructionByName(*function, expected.success);
    const Instruction *failure =
        findInstructionByName(*function, expected.failure);
    const GlobalVariable *lock = module->getNamedGlobal(expected.lock);
    ASSERT_NE(success, nullptr);
    ASSERT_NE(failure, nullptr);
    ASSERT_NE(lock, nullptr);
    EXPECT_EQ(lsa.getMustLockSetAt(success).count(lock), 1u);
    EXPECT_EQ(lsa.getMayLockSetAt(failure).count(lock), 0u);
  }
}

TEST_F(LockSetAnalysisTest, WrappedRwTryLockRefinesZeroSuccessEdge) {
  const char *source = R"(
    declare i32 @__wrap_pthread_rwlock_tryrdlock(i8*)
    @lock = global i8 0
    define void @test() {
    entry:
      %result = call i32 @__wrap_pthread_rwlock_tryrdlock(i8* @lock)
      switch i32 %result, label %failure [ i32 0, label %success ]
    success:
      %at_success = add i32 1, 2
      ret void
    failure:
      %at_failure = add i32 3, 4
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Function *test = module->getFunction("test");
  const Instruction *success = findInstructionByName(*test, "at_success");
  const Instruction *failure = findInstructionByName(*test, "at_failure");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(success, nullptr);
  ASSERT_NE(failure, nullptr);
  EXPECT_EQ(lsa.getMustReadLockSetAt(success).count(lock), 1u);
  EXPECT_EQ(lsa.getMayLockSetAt(failure).count(lock), 0u);
}

TEST_F(LockSetAnalysisTest, LossyTryLockPredicatesDoNotRefineMust) {
  const char *source = R"(
    declare i32 @pthread_mutex_trylock(i8*)
    @trunc_lock = global i8 0
    @xor_lock = global i8 0
    define void @truncated() {
    entry:
      %r = call i32 @pthread_mutex_trylock(i8* @trunc_lock)
      %b = trunc i32 %r to i1
      br i1 %b, label %failure, label %success
    success:
      %trunc_success = add i32 1, 2
      ret void
    failure:
      ret void
    }
    define void @wide_xor() {
    entry:
      %r = call i32 @pthread_mutex_trylock(i8* @xor_lock)
      %x = xor i32 %r, 1
      %b = icmp eq i32 %x, 0
      br i1 %b, label %success, label %failure
    success:
      %xor_success = add i32 3, 4
      ret void
    failure:
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Instruction *trunc_success = findInstructionByName(
      *module->getFunction("truncated"), "trunc_success");
  const Instruction *xor_success =
      findInstructionByName(*module->getFunction("wide_xor"), "xor_success");
  EXPECT_TRUE(lsa.getMustLockSetAt(trunc_success).empty());
  EXPECT_TRUE(lsa.getMustLockSetAt(xor_success).empty());
}

TEST_F(LockSetAnalysisTest, DuplicateTryLockEdgesRemainMayOnly) {
  const char *source = R"(
    declare i32 @pthread_mutex_trylock(i8*)
    @branch_lock = global i8 0
    @switch_lock = global i8 0
    define void @branch_case() {
    entry:
      %r = call i32 @pthread_mutex_trylock(i8* @branch_lock)
      %ok = icmp eq i32 %r, 0
      br i1 %ok, label %join, label %join
    join:
      %branch_join = add i32 1, 2
      ret void
    }
    define void @switch_case() {
    entry:
      %r = call i32 @pthread_mutex_trylock(i8* @switch_lock)
      switch i32 %r, label %join [ i32 0, label %join ]
    join:
      %switch_join = add i32 3, 4
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Instruction *branch_join = findInstructionByName(
      *module->getFunction("branch_case"), "branch_join");
  const Instruction *switch_join = findInstructionByName(
      *module->getFunction("switch_case"), "switch_join");
  const GlobalVariable *branch_lock = module->getNamedGlobal("branch_lock");
  const GlobalVariable *switch_lock = module->getNamedGlobal("switch_lock");
  EXPECT_TRUE(lsa.mayHoldLock(branch_join, branch_lock));
  EXPECT_FALSE(lsa.mustHoldLock(branch_join, branch_lock));
  EXPECT_TRUE(lsa.mayHoldLock(switch_join, switch_lock));
  EXPECT_FALSE(lsa.mustHoldLock(switch_join, switch_lock));
}

