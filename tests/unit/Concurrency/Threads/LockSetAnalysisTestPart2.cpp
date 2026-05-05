#include "LockSetAnalysisTestSupport.h"

TEST_F(LockSetAnalysisTest, SharedLockSummaryPreservesReadModeAcrossCalls) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
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

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_TRUE(lsa.mustHoldLock(after, lock));
  EXPECT_TRUE(lsa.getMayReadLockSetAt(after).count(lock) > 0);
  EXPECT_TRUE(lsa.getMayWriteLockSetAt(after).count(lock) == 0);
  EXPECT_TRUE(lsa.getMustReadLockSetAt(after).count(lock) > 0);
  EXPECT_TRUE(lsa.getMustWriteLockSetAt(after).count(lock) == 0);
}
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
TEST_F(LockSetAnalysisTest, AdoptLockDoesNotSynthesizeAcquisition) {
  const char *source = R"(
    declare void @fake_unique_lock_adopt_lock_C1E(i8*, i8*)

    @lock = global i8 0

    define i32 @main() {
    entry:
      %ul = alloca i8
      call void @fake_unique_lock_adopt_lock_C1E(i8* %ul, i8* @lock)
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

  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
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
TEST_F(LockSetAnalysisTest,
       CalleeHeldExitLocksBecomeCallerMustLocksWhenDefinitelyAcquired) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
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

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_TRUE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest,
       HelperUnlockDropsCallerMustLockStateButPreservesMayWhenConditional) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0
    @flag = external global i1

    define void @maybe_unlock() {
    entry:
      %cond = load i1, i1* @flag
      br i1 %cond, label %unlock, label %done

    unlock:
      call i32 @pthread_mutex_unlock(i8* @lock)
      br label %done

    done:
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      call void @maybe_unlock()
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
TEST_F(LockSetAnalysisTest,
       MixedBalancedAndBareUnlockStillDropsCallerMustLockState) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @helper(i1 %take_internal) {
    entry:
      br i1 %take_internal, label %take, label %release

    take:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %release

    release:
      call i32 @pthread_mutex_unlock(i8* @lock)
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
TEST_F(LockSetAnalysisTest,
       UnresolvedIndirectCallClearsMustLockStateConservatively) {
  const char *source = R"(
    @hook = external global void ()*

    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @unlock_helper() {
    entry:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %fn = load void ()*, void ()** @hook
      call void %fn()
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

  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest,
       RAIILifetimeKeepsExplicitDestructorAndExceptionalExitReleasePoints) {
  const char *source = R"(
    declare void @fake_lock_guard_C1E(i8*, i8*)
    declare void @fake_lock_guard_D1Ev(i8*)
    declare void @may_throw()
    declare i32 @__gxx_personality_v0(...)

    @lock = global i8 0

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %lg = alloca i8
      call void @fake_lock_guard_C1E(i8* %lg, i8* @lock)
      invoke void @may_throw() to label %cont unwind label %lpad

    cont:
      call void @fake_lock_guard_D1Ev(i8* %lg)
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  RAIILock::RAIILockTracker tracker;
  tracker.analyzeFunction(main_func);

  const auto &lifetimes = tracker.getAllLockLifetimes();
  ASSERT_EQ(lifetimes.size(), 1u);

  const auto &lifetime = lifetimes.begin()->second;
  EXPECT_FALSE(lifetime.destructors.empty());

  const Instruction *explicit_dtor = nullptr;
  const Instruction *resume_inst = nullptr;
  for (const auto &bb : *main_func) {
    for (const auto &inst : bb) {
      if (auto *call = dyn_cast<CallBase>(&inst)) {
        if (const Function *callee = call->getCalledFunction()) {
          if (callee->getName().contains("fake_lock_guard_D1Ev")) {
            explicit_dtor = &inst;
          }
        }
      }
      if (isa<ResumeInst>(inst)) {
        resume_inst = &inst;
      }
    }
  }

  ASSERT_NE(explicit_dtor, nullptr);
  ASSERT_NE(resume_inst, nullptr);

  EXPECT_NE(std::find(lifetime.destructors.begin(), lifetime.destructors.end(),
                      explicit_dtor),
            lifetime.destructors.end());
  EXPECT_EQ(std::find(lifetime.destructors.begin(), lifetime.destructors.end(),
                      resume_inst),
            lifetime.destructors.end());

  for (const Instruction *inst : lifetime.destructors) {
    EXPECT_FALSE(isa<ReturnInst>(inst));
  }

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);
  EXPECT_FALSE(lsa.mustHoldLock(resume_inst, lock));
  EXPECT_TRUE(lsa.mayHoldLock(resume_inst, lock));
  EXPECT_EQ(lsa.getLockReleases(lock).size(), 1u);
  EXPECT_EQ(lsa.getStatistics().num_releases, 1u);
}
TEST_F(LockSetAnalysisTest, UnwindFromInnerScopeDoesNotReleaseOuterRaiiLock) {
  const char *source = R"(
    declare void @fake_lock_guard_C1E(i8*, i8*)
    declare void @fake_lock_guard_D1Ev(i8*)
    declare void @may_throw()
    declare i32 @__gxx_personality_v0(...)

    @outer = global i8 0
    @inner = global i8 0

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %outer_guard = alloca i8
      %inner_guard = alloca i8
      call void @fake_lock_guard_C1E(i8* %outer_guard, i8* @outer)
      call void @fake_lock_guard_C1E(i8* %inner_guard, i8* @inner)
      invoke void @may_throw() to label %cont unwind label %lpad

    cont:
      call void @fake_lock_guard_D1Ev(i8* %inner_guard)
      call void @fake_lock_guard_D1Ev(i8* %outer_guard)
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *resume_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (isa<ResumeInst>(&inst)) {
      resume_inst = &inst;
      break;
    }
  }
  ASSERT_NE(resume_inst, nullptr);

  const GlobalVariable *outer = module->getNamedGlobal("outer");
  const GlobalVariable *inner = module->getNamedGlobal("inner");
  ASSERT_NE(outer, nullptr);
  ASSERT_NE(inner, nullptr);

  EXPECT_FALSE(lsa.mustHoldLock(resume_inst, outer));
  EXPECT_TRUE(lsa.mayHoldLock(resume_inst, outer));
  EXPECT_TRUE(lsa.mayHoldLock(resume_inst, inner));
}
TEST_F(LockSetAnalysisTest, ImplicitRaiiUnwindExitClearsMustLockState) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @might_throw()
    declare i32 @__gxx_personality_v0(...)

    @lock = global i8 0

    define void @test() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %ul = alloca i8
      call void @fake_unique_lock_C1E(i8* %ul, i8* @lock)
      invoke void @might_throw()
              to label %cont unwind label %lpad

    cont:
      ret void

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *test_func = module->getFunction("test");
  ASSERT_NE(test_func, nullptr);

  const Instruction *resume_inst = nullptr;
  for (const Instruction &inst : instructions(test_func)) {
    if (isa<ResumeInst>(&inst)) {
      resume_inst = &inst;
      break;
    }
  }
  ASSERT_NE(resume_inst, nullptr);

  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);

  EXPECT_FALSE(lsa.mustHoldLock(resume_inst, lock));
}
TEST_F(LockSetAnalysisTest, BalancedRaiiHelperDoesNotClearCallerMustLockState) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @fake_unique_lock_D1Ev(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      %ul = alloca i8
      call void @fake_unique_lock_C1E(i8* %ul, i8* @lock)
      call void @fake_unique_lock_D1Ev(i8* %ul)
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

  EXPECT_TRUE(lsa.mustHoldLock(after, lock));
}
TEST_F(LockSetAnalysisTest,
       LeadingNonCallBeforeHelperAcquireStillAppliesSummary) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define void @lock_helper() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      %seed = add i32 0, 1
      call void @lock_helper()
      %after = add i32 %seed, 2
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
}
TEST_F(LockSetAnalysisTest,
       LeadingNonCallBeforeHelperReleaseClearsCallerMustState) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @unlock_helper() {
    entry:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %seed = add i32 0, 1
      call void @unlock_helper()
      %after = add i32 %seed, 2
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
TEST_F(LockSetAnalysisTest, HeapAllocatedUniqueLockKeepsUnderlyingMutex) {
  const char *source = R"(
    declare noalias i8* @malloc(i64)
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @fake_unique_lock_D1Ev(i8*)

    @lock = global i8 0

    define i32 @main() {
    entry:
      %ul = call i8* @malloc(i64 8)
      call void @fake_unique_lock_C1E(i8* %ul, i8* @lock)
      %inside = add i32 1, 2
      call void @fake_unique_lock_D1Ev(i8* %ul)
      %after = add i32 %inside, 3
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *inside =
      findInstructionByName(*module->getFunction("main"), "inside");
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(inside, nullptr);
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(inside, lock));
  EXPECT_TRUE(lsa.mustHoldLock(inside, lock));
  EXPECT_FALSE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}
