#include "LockSetAnalysisTestSupport.h"

TEST_F(LockSetAnalysisTest, BlockHeadMustReadLockSetUsesPredecessorMeet) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)

    @lock = global i8 0

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      br label %merge

    right:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      br label %merge

    merge:
      %access = add i32 1, 2
      ret i32 %access
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *access =
      findInstructionByName(*module->getFunction("main"), "access");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(access, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.getMustReadLockSetAt(access).count(lock) > 0);
}

TEST_F(LockSetAnalysisTest, BlockHeadMustWriteLockSetUsesPredecessorMeet) {
  const char *source = R"(
    declare i32 @pthread_rwlock_wrlock(i8*)

    @lock = global i8 0

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      call i32 @pthread_rwlock_wrlock(i8* @lock)
      br label %merge

    right:
      call i32 @pthread_rwlock_wrlock(i8* @lock)
      br label %merge

    merge:
      %access = add i32 1, 2
      ret i32 %access
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *access =
      findInstructionByName(*module->getFunction("main"), "access");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(access, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.getMustWriteLockSetAt(access).count(lock) > 0);
}

TEST_F(LockSetAnalysisTest, ReaderWriterModesNeedAccessSensitiveExclusion) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)

    @lock = global i8 0

    define void @reader() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      %read_access = add i32 1, 2
      ret void
    }

    define void @writer() {
    entry:
      call i32 @pthread_rwlock_wrlock(i8* @lock)
      %write_access = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *read_access =
      findInstructionByName(*module->getFunction("reader"), "read_access");
  const Instruction *write_access =
      findInstructionByName(*module->getFunction("writer"), "write_access");
  ASSERT_NE(read_access, nullptr);
  ASSERT_NE(write_access, nullptr);

  EXPECT_FALSE(lsa.mayHoldCommonLock(read_access, write_access));
  EXPECT_FALSE(lsa.mustHoldCommonLock(read_access, write_access));
  EXPECT_TRUE(lsa.mustMutuallyExclude(read_access, MemoryAccessKind::Read,
                                      write_access, MemoryAccessKind::Write));
}

TEST_F(LockSetAnalysisTest,
       AccessSensitiveMutualExclusionDistinguishesReadReadAndWriteRead) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)

    @lock = global i8 0

    define void @reader1() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      %read1 = add i32 1, 2
      ret void
    }

    define void @reader2() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      %read2 = add i32 3, 4
      ret void
    }

    define void @writer() {
    entry:
      call i32 @pthread_rwlock_wrlock(i8* @lock)
      %write = add i32 5, 6
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *read1 =
      findInstructionByName(*module->getFunction("reader1"), "read1");
  const Instruction *read2 =
      findInstructionByName(*module->getFunction("reader2"), "read2");
  const Instruction *write =
      findInstructionByName(*module->getFunction("writer"), "write");
  ASSERT_NE(read1, nullptr);
  ASSERT_NE(read2, nullptr);
  ASSERT_NE(write, nullptr);

  EXPECT_FALSE(lsa.mustMutuallyExclude(read1, MemoryAccessKind::Read, read2,
                                       MemoryAccessKind::Read));
  EXPECT_TRUE(lsa.mustMutuallyExclude(read1, MemoryAccessKind::Read, write,
                                      MemoryAccessKind::Write));
}

TEST_F(LockSetAnalysisTest, OpenMPCriticalUsesNamedAnalysisIdentity) {
  const char *source = R"(
    @crit = global [8 x i32] zeroinitializer

    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define i32 @main() {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      %inside = add i32 1, 2
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret i32 %inside
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *inside = findInstructionByName(*main_func, "inside");
  ASSERT_NE(inside, nullptr);

  const GlobalVariable *crit = module->getNamedGlobal("crit");
  ASSERT_NE(crit, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(inside, crit));
  EXPECT_TRUE(lsa.mustHoldLock(inside, crit));
  EXPECT_EQ(lsa.getLockAcquires(crit).size(), 1u);
  EXPECT_EQ(lsa.getLockReleases(crit).size(), 1u);
}

TEST_F(LockSetAnalysisTest, DistinctLockFieldsDoNotCollapseToSharedBase) {
  const char *source = R"(
    %struct.Locks = type { i8, i8 }

    declare i32 @pthread_mutex_lock(i8*)

    @locks = global %struct.Locks zeroinitializer

    define void @worker1() {
    entry:
      %lock1 = getelementptr inbounds %struct.Locks, %struct.Locks* @locks,
                                   i32 0, i32 0
      call i32 @pthread_mutex_lock(i8* %lock1)
      %access1 = add i32 1, 2
      ret void
    }

    define void @worker2() {
    entry:
      %lock2 = getelementptr inbounds %struct.Locks, %struct.Locks* @locks,
                                   i32 0, i32 1
      call i32 @pthread_mutex_lock(i8* %lock2)
      %access2 = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  lotus::AliasAnalysisWrapper aa(*module, lotus::AAConfig::SparrowAA_NoCtx());
  LockSetAnalysis lsa(*module);
  lsa.setAliasAnalysis(&aa);
  lsa.analyze();

  const Instruction *access1 =
      findInstructionByName(*module->getFunction("worker1"), "access1");
  const Instruction *access2 =
      findInstructionByName(*module->getFunction("worker2"), "access2");
  ASSERT_NE(access1, nullptr);
  ASSERT_NE(access2, nullptr);

  LockSet locks1 = lsa.getMayWriteLockSetAt(access1);
  LockSet locks2 = lsa.getMayWriteLockSetAt(access2);
  ASSERT_EQ(locks1.size(), 1u);
  ASSERT_EQ(locks2.size(), 1u);
  EXPECT_NE(*locks1.begin(), *locks2.begin());
  EXPECT_FALSE(lsa.mustHoldCommonLock(access1, access2));
}

TEST_F(LockSetAnalysisTest, UnknownReleaseAliasKillsMustWithoutAA) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define void @test(i8* %p, i8* %q) {
    entry:
      call i32 @pthread_mutex_lock(i8* %p)
      call i32 @pthread_mutex_unlock(i8* %q)
      %after = add i32 1, 2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *test = module->getFunction("test");
  const Instruction *after = findInstructionByName(*test, "after");
  const Argument *p = test->getArg(0);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(lsa.getMustLockSetAt(after).count(p), 0u);
}

TEST_F(LockSetAnalysisTest, WriteAcquirePreservesUnrelatedMayReadLock) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)

    @read_lock = global i8 0
    @write_lock = global i8 0

    define void @test() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @read_lock)
      call i32 @pthread_rwlock_wrlock(i8* @write_lock)
      %after = add i32 1, 2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *test = module->getFunction("test");
  const Instruction *after = findInstructionByName(*test, "after");
  const GlobalVariable *read_lock = module->getNamedGlobal("read_lock");
  const GlobalVariable *write_lock = module->getNamedGlobal("write_lock");
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(lsa.getMayReadLockSetAt(after).count(read_lock), 1u);
  EXPECT_EQ(lsa.getMayWriteLockSetAt(after).count(write_lock), 1u);
}

TEST_F(LockSetAnalysisTest, RecursiveMutexTracksSaturatingDepth) {
  const char *source = R"(
    declare void @_ZNSt15recursive_mutex4lockEv(i8*)
    declare void @_ZNSt15recursive_mutex6unlockEv(i8*)

    @recursive = global i8 0

    define void @test() {
    entry:
      call void @_ZNSt15recursive_mutex4lockEv(i8* @recursive)
      call void @_ZNSt15recursive_mutex4lockEv(i8* @recursive)
      %twice = add i32 1, 2
      call void @_ZNSt15recursive_mutex6unlockEv(i8* @recursive)
      %once = add i32 3, 4
      call void @_ZNSt15recursive_mutex6unlockEv(i8* @recursive)
      %released = add i32 5, 6
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *test = module->getFunction("test");
  const Instruction *twice = findInstructionByName(*test, "twice");
  const Instruction *once = findInstructionByName(*test, "once");
  const Instruction *released = findInstructionByName(*test, "released");
  const GlobalVariable *recursive = module->getNamedGlobal("recursive");
  ASSERT_NE(twice, nullptr);
  ASSERT_NE(once, nullptr);
  ASSERT_NE(released, nullptr);
  ASSERT_NE(recursive, nullptr);

  EXPECT_EQ(lsa.getLockNestingDepth(twice), 2u);
  EXPECT_TRUE(lsa.mustHoldLock(once, recursive));
  EXPECT_EQ(lsa.getLockNestingDepth(once), 1u);
  EXPECT_FALSE(lsa.mayHoldLock(released, recursive));
}

TEST_F(LockSetAnalysisTest, LoadedPointerSlotsDoNotPreserveMustAfterUnlock) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)
    @mutex = global i8 0
    @p = global i8* @mutex
    @q = global i8* @mutex
    define void @test() {
    entry:
      %a = load i8*, i8** @p
      call i32 @pthread_mutex_lock(i8* %a)
      %b = load i8*, i8** @q
      call i32 @pthread_mutex_unlock(i8* %b)
      %after = add i32 1, 2
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Instruction *after =
      findInstructionByName(*module->getFunction("test"), "after");
  EXPECT_TRUE(lsa.getMustLockSetAt(after).empty());
}

