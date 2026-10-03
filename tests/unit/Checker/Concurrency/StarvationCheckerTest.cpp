#include "Checker/Concurrency/StarvationChecker.h"

#include "ConcurrencyCheckerTestSupport.h"

namespace {
class StarvationCheckerTest : public ConcurrencyCheckerTest {
protected:
  std::vector<concurrency::ConcurrencyBugReport> run(const char *ir) {
    auto module = parseModule(ir);
    EXPECT_NE(module, nullptr);
    if (!module)
      return {};
    mhp::LockSetAnalysis locks(*module);
    locks.analyze();
    concurrency::StarvationChecker checker(*module, locks);
    return checker.checkStarvation();
  }
};
} // namespace

TEST_F(StarvationCheckerTest, ReportsBlockingWhileHoldingMutex) {
  auto reports = run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare i32 @sleep(i32)
    define void @bad(i8* %m) {
      call void @pthread_mutex_lock(i8* %m)
      call i32 @sleep(i32 1)
      ret void
    }
  )");
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_NE(reports.front().description.find("Blocking call while holding"),
            std::string::npos);
  EXPECT_GE(reports.front().steps.size(), 2u);
}

TEST_F(StarvationCheckerTest, UnlockBeforeBlockingIsSafe) {
  EXPECT_TRUE(run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare void @pthread_mutex_unlock(i8*)
    declare i32 @sleep(i32)
    define void @ok(i8* %m) {
      call void @pthread_mutex_lock(i8* %m)
      call void @pthread_mutex_unlock(i8* %m)
      call i32 @sleep(i32 1)
      ret void
    }
  )")
                  .empty());
}

TEST_F(StarvationCheckerTest, PropagatesBlockingThroughHelpers) {
  auto reports = run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare i32 @sleep(i32)
    define void @helper() {
      call i32 @sleep(i32 1)
      ret void
    }
    define void @bad(i8* %m) {
      call void @pthread_mutex_lock(i8* %m)
      call void @helper()
      ret void
    }
  )");
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_GE(reports.front().steps.size(), 3u);
}

TEST_F(StarvationCheckerTest, SubstitutesCalleeReleaseBeforeBlocking) {
  EXPECT_TRUE(run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare void @pthread_mutex_unlock(i8*)
    declare i32 @sleep(i32)
    define void @helper(i8* %m) {
      call void @pthread_mutex_unlock(i8* %m)
      call i32 @sleep(i32 1)
      ret void
    }
    define void @ok(i8* %m) {
      call void @pthread_mutex_lock(i8* %m)
      call void @helper(i8* %m)
      ret void
    }
  )")
                  .empty());
}

TEST_F(StarvationCheckerTest, AppliesReleaseOnCalleeReturn) {
  EXPECT_TRUE(run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare void @pthread_mutex_unlock(i8*)
    declare i32 @sleep(i32)
    define void @release(i8* %m) {
      call void @pthread_mutex_unlock(i8* %m)
      ret void
    }
    define void @ok(i8* %m) {
      call void @pthread_mutex_lock(i8* %m)
      call void @release(i8* %m)
      call i32 @sleep(i32 1)
      ret void
    }
  )")
                  .empty());
}

TEST_F(StarvationCheckerTest, ConditionWaitReleasesItsMutex) {
  EXPECT_TRUE(run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)
    define void @ok(i8* %m, i8* %c) {
      call void @pthread_mutex_lock(i8* %m)
      call i32 @pthread_cond_wait(i8* %c, i8* %m)
      ret void
    }
  )")
                  .empty());
}

TEST_F(StarvationCheckerTest, ConditionWaitHoldingAnotherMutexIsReported) {
  EXPECT_EQ(run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    define void @bad() {
      call void @pthread_mutex_lock(i8* @a)
      call void @pthread_mutex_lock(i8* @b)
      call i32 @pthread_cond_wait(i8* @c, i8* @b)
      ret void
    }
  )")
                .size(),
            1u);
}

TEST_F(StarvationCheckerTest, ReportsArbitraryCallbackUnderLock) {
  EXPECT_EQ(run(R"(
    declare void @pthread_mutex_lock(i8*)
    define void @bad(i8* %m, void ()* %callback) {
      call void @pthread_mutex_lock(i8* %m)
      call void %callback()
      ret void
    }
  )")
                .size(),
            1u);
}

TEST_F(StarvationCheckerTest, CallbackOutsideLockIsSafe) {
  EXPECT_TRUE(run(R"(
    define void @ok(void ()* %callback) {
      call void %callback()
      ret void
    }
  )")
                  .empty());
}

TEST_F(StarvationCheckerTest, RefinesFailedTryLockPath) {
  EXPECT_TRUE(run(R"(
    declare i32 @pthread_mutex_trylock(i8*)
    declare i32 @sleep(i32)
    @mutex = global i8 0
    define void @ok() {
      %r = call i32 @pthread_mutex_trylock(i8* @mutex)
      %success = icmp eq i32 %r, 0
      br i1 %success, label %locked, label %failed
    locked:
      ret void
    failed:
      call i32 @sleep(i32 1)
      ret void
    }
  )")
                  .empty());
}

TEST_F(StarvationCheckerTest, TryLockOnUIThreadIsNonblocking) {
  EXPECT_TRUE(run(R"(
    declare i32 @pthread_mutex_trylock(i8*)
    @mutex = global i8 0
    define void @ok() "lotus.ui-thread" {
      call i32 @pthread_mutex_trylock(i8* @mutex)
      ret void
    }
  )")
                  .empty());
}

TEST_F(StarvationCheckerTest, ReleasesRAIILockAtDestructor) {
  EXPECT_EQ(run(R"(
    declare void @fake_lock_guard_C1E(i8*, i8*)
    declare void @fake_lock_guard_D1Ev(i8*)
    declare i32 @sleep(i32)
    @mutex = global i8 0
    define void @test() {
      %guard = alloca i8
      call void @fake_lock_guard_C1E(i8* %guard, i8* @mutex)
      call i32 @sleep(i32 1)
      call void @fake_lock_guard_D1Ev(i8* %guard)
      call i32 @sleep(i32 2)
      ret void
    }
  )")
                .size(),
            1u);
}

TEST_F(StarvationCheckerTest, HonorsCustomBlockingAttribute) {
  EXPECT_EQ(run(R"(
    declare void @wait_for_reply() "lotus.may-block"
    define void @ui() "lotus.ui-thread" {
      call void @wait_for_reply()
      ret void
    }
  )")
                .size(),
            1u);
}

TEST_F(StarvationCheckerTest, PropagatesUIThreadAndLocklessContracts) {
  auto reports = run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare i32 @sleep(i32)
    define void @helper() {
      call i32 @sleep(i32 1)
      ret void
    }
    define void @ui() "lotus.ui-thread" {
      call void @helper()
      ret void
    }
    define void @lockless(i8* %m) "lotus.lockless" {
      call void @pthread_mutex_lock(i8* %m)
      ret void
    }
  )");
  EXPECT_EQ(reports.size(), 2u);
}

TEST_F(StarvationCheckerTest, IgnoresConstantUnreachableBlocking) {
  EXPECT_TRUE(run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare i32 @sleep(i32)
    define void @ok(i8* %m) {
      call void @pthread_mutex_lock(i8* %m)
      br i1 false, label %dead, label %exit
    dead:
      call i32 @sleep(i32 1)
      ret void
    exit:
      ret void
    }
  )")
                  .empty());
}

TEST_F(StarvationCheckerTest, RecursiveSummariesTerminate) {
  EXPECT_FALSE(run(R"(
    declare void @pthread_mutex_lock(i8*)
    declare i32 @sleep(i32)
    define void @recur(i1 %b) {
      br i1 %b, label %again, label %stop
    again:
      call void @recur(i1 false)
      ret void
    stop:
      call i32 @sleep(i32 1)
      ret void
    }
    define void @bad(i8* %m) {
      call void @pthread_mutex_lock(i8* %m)
      call void @recur(i1 true)
      ret void
    }
  )")
                   .empty());
}
