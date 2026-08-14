#include "LockSetAnalysisTestSupport.h"

TEST_F(LockSetAnalysisTest, DeferredDestructorPreservesIndependentRawLock) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare void @fake_unique_lock_defer_lock_C1E(i8*, i8*, i8*)
    declare void @fake_unique_lock_D1Ev(i8*)
    @lock = global i8 0
    @tag = global i8 0
    define void @test() {
      %u = alloca i8
      call i32 @pthread_mutex_lock(i8* @lock)
      call void @fake_unique_lock_defer_lock_C1E(i8* %u, i8* @lock, i8* @tag)
      call void @fake_unique_lock_D1Ev(i8* %u)
      %after = add i32 1, 2
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  EXPECT_TRUE(lsa.mustHoldLock(
      findInstructionByName(*module->getFunction("test"), "after"),
      module->getNamedGlobal("lock")));
}

TEST_F(LockSetAnalysisTest, UniqueLockReleaseKeepsMutexHeldPastDestructor) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare i8* @_ZNSt11unique_lockISt5mutexE7releaseEv(i8*)
    declare void @fake_unique_lock_D1Ev(i8*)
    @lock = global i8 0
    define void @test() {
      %u = alloca i8
      call void @fake_unique_lock_C1E(i8* %u, i8* @lock)
      call i8* @_ZNSt11unique_lockISt5mutexE7releaseEv(i8* %u)
      call void @fake_unique_lock_D1Ev(i8* %u)
      %after = add i32 1, 2
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  EXPECT_TRUE(lsa.mustHoldLock(
      findInstructionByName(*module->getFunction("test"), "after"),
      module->getNamedGlobal("lock")));
}

TEST_F(LockSetAnalysisTest,
       UniqueLockMoveTransfersOwnershipToDestinationDestructor) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @_ZNSt11unique_lockISt5mutexEC1EOS2_(i8*, i8*)
    declare void @fake_unique_lock_D1Ev(i8*)
    @lock = global i8 0
    define void @test() {
      %source = alloca i8
      %destination = alloca i8
      call void @fake_unique_lock_C1E(i8* %source, i8* @lock)
      call void @_ZNSt11unique_lockISt5mutexEC1EOS2_(
          i8* %destination, i8* %source)
      call void @fake_unique_lock_D1Ev(i8* %destination)
      %after_destination = add i32 1, 2
      call void @fake_unique_lock_D1Ev(i8* %source)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  EXPECT_FALSE(lsa.mayHoldLock(
      findInstructionByName(*module->getFunction("test"),
                            "after_destination"),
      module->getNamedGlobal("lock")));
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

TEST_F(LockSetAnalysisTest, ReusedRaiiStorageUsesReachableLifetime) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @fake_unique_lock_D1Ev(i8*)

    @a = global i8 0
    @b = global i8 0

    define void @test(i1 %choose_a) {
    entry:
      %wrapper = alloca i8
      br i1 %choose_a, label %a_path, label %b_path

    a_path:
      call void @fake_unique_lock_C1E(i8* %wrapper, i8* @a)
      %inside_a = add i32 1, 2
      call void @fake_unique_lock_D1Ev(i8* %wrapper)
      %after_a = add i32 3, 4
      ret void

    b_path:
      call void @fake_unique_lock_C1E(i8* %wrapper, i8* @b)
      %inside_b = add i32 5, 6
      call void @fake_unique_lock_D1Ev(i8* %wrapper)
      %after_b = add i32 7, 8
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *test = module->getFunction("test");
  const Instruction *inside_a = findInstructionByName(*test, "inside_a");
  const Instruction *inside_b = findInstructionByName(*test, "inside_b");
  const Instruction *after_a = findInstructionByName(*test, "after_a");
  const Instruction *after_b = findInstructionByName(*test, "after_b");
  const GlobalVariable *a = module->getNamedGlobal("a");
  const GlobalVariable *b = module->getNamedGlobal("b");

  EXPECT_TRUE(lsa.mustHoldLock(inside_a, a));
  EXPECT_FALSE(lsa.mayHoldLock(inside_a, b));
  EXPECT_TRUE(lsa.mustHoldLock(inside_b, b));
  EXPECT_FALSE(lsa.mayHoldLock(inside_b, a));
  EXPECT_FALSE(lsa.mayHoldLock(after_a, a));
  EXPECT_FALSE(lsa.mayHoldLock(after_b, b));
}

TEST_F(LockSetAnalysisTest, BranchSensitiveUniqueLockReleaseAtDestructor) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare void @fake_unique_lock_C1E_adopt_lock(i8*, i8*, i8*)
    declare i8* @fake_unique_lock_7releaseEv(i8*)
    declare void @fake_unique_lock_D1Ev(i8*)
    @mutex = global i8 0
    define void @test(i1 %detach) {
    entry:
      %wrapper = alloca i8
      call i32 @pthread_mutex_lock(i8* @mutex)
      call void @fake_unique_lock_C1E_adopt_lock(i8* %wrapper, i8* @mutex,
                                                 i8* null)
      br i1 %detach, label %release, label %join
    release:
      call i8* @fake_unique_lock_7releaseEv(i8* %wrapper)
      br label %join
    join:
      call void @fake_unique_lock_D1Ev(i8* %wrapper)
      %after = add i32 1, 2
      ret void
    }
    define void @test_permuted(i1 %detach) {
    entry:
      %wrapper = alloca i8
      call i32 @pthread_mutex_lock(i8* @mutex)
      call void @fake_unique_lock_C1E_adopt_lock(i8* %wrapper, i8* @mutex,
                                                 i8* null)
      br i1 %detach, label %release, label %join
    join:
      call void @fake_unique_lock_D1Ev(i8* %wrapper)
      %after_permuted = add i32 3, 4
      ret void
    release:
      call i8* @fake_unique_lock_7releaseEv(i8* %wrapper)
      br label %join
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Instruction *after =
      findInstructionByName(*module->getFunction("test"), "after");
  const GlobalVariable *mutex = module->getNamedGlobal("mutex");
  const Instruction *after_permuted = findInstructionByName(
      *module->getFunction("test_permuted"), "after_permuted");
  EXPECT_TRUE(lsa.mayHoldLock(after, mutex));
  EXPECT_FALSE(lsa.mustHoldLock(after, mutex));
  EXPECT_TRUE(lsa.mayHoldLock(after_permuted, mutex));
  EXPECT_FALSE(lsa.mustHoldLock(after_permuted, mutex));
}

