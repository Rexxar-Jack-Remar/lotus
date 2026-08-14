#include "LockSetAnalysisTestSupport.h"

TEST_F(LockSetAnalysisTest, SharedReadOrderDoesNotCreateDeadlockCycle) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)

    @a = global i8 0
    @b = global i8 0

    define void @first() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @a)
      call i32 @pthread_rwlock_rdlock(i8* @b)
      ret void
    }

    define void @second() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @b)
      call i32 @pthread_rwlock_rdlock(i8* @a)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  EXPECT_TRUE(lsa.detectLockOrderInversions().empty());
  EXPECT_TRUE(lsa.detectDeadlockCycles().empty());
}

TEST_F(LockSetAnalysisTest, TryLockDoesNotCreateDeadlockWaitEdge) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_trylock(i8*)

    @a = global i8 0
    @b = global i8 0

    define void @first() {
    entry:
      call i32 @pthread_mutex_lock(i8* @a)
      call i32 @pthread_mutex_trylock(i8* @b)
      ret void
    }

    define void @second() {
    entry:
      call i32 @pthread_mutex_lock(i8* @b)
      call i32 @pthread_mutex_lock(i8* @a)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  EXPECT_TRUE(lsa.detectLockOrderInversions().empty());
  EXPECT_TRUE(lsa.detectDeadlockCycles().empty());
}

TEST_F(LockSetAnalysisTest, IncompatibleBlockingOrderCreatesDeadlockCycle) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)

    @a = global i8 0
    @b = global i8 0

    define void @first() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @a)
      call i32 @pthread_rwlock_wrlock(i8* @b)
      ret void
    }

    define void @second() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @b)
      call i32 @pthread_rwlock_wrlock(i8* @a)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  EXPECT_EQ(lsa.detectLockOrderInversions().size(), 1u);
  EXPECT_FALSE(lsa.detectDeadlockCycles().empty());
}

TEST_F(LockSetAnalysisTest, FailedTryDoesNotCreateSubsequentOrderEdge) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_trylock(i8*)

    @a = global i8 0
    @b = global i8 0
    @c = global i8 0

    define void @test() {
    entry:
      call i32 @pthread_mutex_lock(i8* @a)
      %result = call i32 @pthread_mutex_trylock(i8* @b)
      %failed = icmp ne i32 %result, 0
      br i1 %failed, label %failure, label %success

    failure:
      call i32 @pthread_mutex_lock(i8* @c)
      ret void

    success:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const GlobalVariable *b = module->getNamedGlobal("b");
  const GlobalVariable *c = module->getNamedGlobal("c");
  const auto successors = lsa.getLockOrderSuccessors(b);
  EXPECT_EQ(std::find(successors.begin(), successors.end(), c),
            successors.end());
}

