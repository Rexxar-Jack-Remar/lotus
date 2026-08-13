#include "HappensBeforeAnalysisTestSupport.h"

TEST_F(HappensBeforeAnalysisTest, TwoThreadsNoSync_NeitherHappensBefore) {
  const char *source = R"(
    @x = global i32 0, align 4
    @y = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @thread_a(i8* %arg) {
    entry:
      store i32 1, i32* @x, align 4
      ret i8* null
    }

    define i8* @thread_b(i8* %arg) {
    entry:
      store i32 2, i32* @y, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @thread_a, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @thread_b, i8* null)
      call i32 @pthread_join(i8* %tid1, i8* null)
      call i32 @pthread_join(i8* %tid2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *thread_a = module->getFunction("thread_a");
  const Function *thread_b = module->getFunction("thread_b");
  ASSERT_NE(thread_a, nullptr);
  ASSERT_NE(thread_b, nullptr);

  const Instruction *store_a = &thread_a->getEntryBlock().front();
  const Instruction *store_b = &thread_b->getEntryBlock().front();
  ASSERT_NE(store_a, nullptr);
  ASSERT_NE(store_b, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_a, store_b));
  EXPECT_FALSE(hb.happensBefore(store_b, store_a));
  EXPECT_FALSE(hb.happensBefore(store_a, store_a));
}

TEST_F(HappensBeforeAnalysisTest,
       StaticMustPrecedeRequiresEveryCallContextPair) {
  const char *source = R"(
    define void @fa() {
    entry:
      %a = add i32 1, 2
      ret void
    }
    define void @fb() {
    entry:
      %b = add i32 3, 4
      ret void
    }
    define i32 @main() {
    entry:
      call void @fa()
      call void @fb()
      call void @fb()
      call void @fa()
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();
  const Instruction *a = findInstructionByName(*module->getFunction("fa"), "a");
  const Instruction *b = findInstructionByName(*module->getFunction("fb"), "b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_FALSE(hb.mustPrecede(b, a));
  EXPECT_FALSE(hb.mustPrecede(a, b));
}

TEST_F(HappensBeforeAnalysisTest, StaleAnalysisGenerationFailsClosed) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %a = add i32 1, 2
      %b = add i32 %a, 3
      ret i32 %b
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();
  const Function *main_func = module->getFunction("main");
  const Instruction *a = findInstructionByName(*main_func, "a");
  const Instruction *b = findInstructionByName(*main_func, "b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_TRUE(hb.mustPrecede(a, b));
  mhp.analyze();
  EXPECT_FALSE(hb.mustPrecede(a, b));
}
TEST_F(HappensBeforeAnalysisTest, MultiExitWorkerStillHappensBeforePostJoin) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %cond = icmp eq i8* %arg, null
      br i1 %cond, label %left, label %right

    left:
      %left_work = add i32 1, 2
      ret i8* null

    right:
      %right_work = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* undef)
      call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *worker_func = module->getFunction("worker");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(worker_func, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *left_work =
      findInstructionByName(*worker_func, "left_work");
  const Instruction *right_work =
      findInstructionByName(*worker_func, "right_work");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(left_work, nullptr);
  ASSERT_NE(right_work, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_TRUE(hb.happensBefore(left_work, post));
  EXPECT_TRUE(hb.happensBefore(right_work, post));
}
TEST_F(HappensBeforeAnalysisTest, MutexHandoffAcrossForkCreatesHB) {
  const char *source = R"(
    @lock = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define i8* @worker(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %seen = load i32, i32* @shared, align 4
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_mutex_lock(i8* @lock)
      store i32 42, i32* @shared, align 4
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_mutex_unlock(i8* @lock)
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *store_shared = nullptr;
  for (const Instruction &inst : instructions(*main_func)) {
    if (isa<StoreInst>(&inst)) {
      store_shared = &inst;
      break;
    }
  }
  const Instruction *load_shared = findInstructionByName(*worker_func, "seen");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       CompetingPeerMutexCriticalSectionsDoNotInventHB) {
  const char *source = R"(
    @lock = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      store i32 1, i32* @shared, align 4
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %seen = load i32, i32* @shared, align 4
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store_shared = &inst;
      break;
    }
  }
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("worker2"), "seen");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
  EXPECT_FALSE(hb.happensBefore(load_shared, store_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       UniqueConditionSignalDoesNotCreateDefiniteHB) {
  const char *source = R"(
    @cond = global i8 0
    @mutex = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)
    declare i32 @pthread_cond_signal(i8*)

    define i8* @waiter(i8* %arg) {
    entry:
      call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %seen = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @signaler(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      call i32 @pthread_cond_signal(i8* @cond)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @waiter, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @signaler, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("signaler")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("waiter"), "seen");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("condvar_relations_deferred");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}
TEST_F(HappensBeforeAnalysisTest,
       MultipleWaitersOnSameConditionRemainDeferred) {
  const char *source = R"(
    @cond = global i8 0
    @mutex = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)
    declare i32 @pthread_cond_signal(i8*)

    define i8* @waiter1(i8* %arg) {
    entry:
      call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %seen1 = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @waiter2(i8* %arg) {
    entry:
      call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %seen2 = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @signaler(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      call i32 @pthread_cond_signal(i8* @cond)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @waiter1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @waiter2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @signaler, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("signaler")->getEntryBlock().front();
  const Instruction *load1 =
      findInstructionByName(*module->getFunction("waiter1"), "seen1");
  const Instruction *load2 =
      findInstructionByName(*module->getFunction("waiter2"), "seen2");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load1, nullptr);
  ASSERT_NE(load2, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load1));
  EXPECT_FALSE(hb.happensBefore(store_shared, load2));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("condvar_relations_deferred");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}
TEST_F(HappensBeforeAnalysisTest, CallOnceDoesNotCreateBidirectionalHB) {
  const char *source = R"(
    @flag = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag)
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag)
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *worker1 = module->getFunction("worker1");
  const Function *worker2 = module->getFunction("worker2");
  ASSERT_NE(worker1, nullptr);
  ASSERT_NE(worker2, nullptr);

  const Instruction *w1 = findInstructionByName(*worker1, "w1");
  const Instruction *w2 = findInstructionByName(*worker2, "w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  EXPECT_FALSE(hb.happensBefore(w1, w2) && hb.happensBefore(w2, w1));
}
TEST_F(HappensBeforeAnalysisTest, CallOnceCallbackSynchronizesWithFollowers) {
  const char *source = R"(
    @flag = global i8 0
    @shared = global i32 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*, void ()*)

    define void @init_once() {
    entry:
      store i32 7, i32* @shared
      ret void
    }

    define i8* @worker1(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_once)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_once)
      %v = load i32, i32* @shared
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *init_once = module->getFunction("init_once");
  const Function *worker2 = module->getFunction("worker2");
  ASSERT_NE(init_once, nullptr);
  ASSERT_NE(worker2, nullptr);

  const Instruction *store_shared = &init_once->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*worker2, "v");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       CallOnceWithDifferentCallbacksDoesNotInventCrossCallbackHB) {
  const char *source = R"(
    @flag = global i8 0
    @a = global i32 0
    @b = global i32 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*, void ()*)

    define void @init_a() {
    entry:
      store i32 7, i32* @a
      ret void
    }

    define void @init_b() {
    entry:
      store i32 9, i32* @b
      ret void
    }

    define i8* @worker1(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_a)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_b)
      %v = load i32, i32* @a
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_a =
      &module->getFunction("init_a")->getEntryBlock().front();
  const Instruction *load_a =
      findInstructionByName(*module->getFunction("worker2"), "v");
  ASSERT_NE(store_a, nullptr);
  ASSERT_NE(load_a, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_a, load_a));
}
TEST_F(HappensBeforeAnalysisTest,
       ReusedCallOnceCallbackDoesNotLeakHBToDirectCalls) {
  const char *source = R"(
    @flag = global i8 0
    @shared = global i32 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*, void ()*)

    define void @init_once() {
    entry:
      store i32 7, i32* @shared
      ret void
    }

    define i8* @once_worker(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_once)
      ret i8* null
    }

    define i8* @direct_worker(i8* %arg) {
    entry:
      call void @init_once()
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_once)
      %v = load i32, i32* @shared
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @once_worker, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @direct_worker, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *init_once = module->getFunction("init_once");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(init_once, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_shared = &init_once->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader, "v");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest, PromiseFutureTracksSharedState) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @promise_obj = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @_ZNSt7promise10get_futureEv(i8*)
    declare void @_ZNSt7promise9set_valueEv(i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 99, i32* @shared, align 4
      call void @_ZNSt7promise9set_valueEv(i8* @promise_obj)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      %future = call i8* @_ZNSt7promise10get_futureEv(i8* @promise_obj)
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest, AsyncFutureGetOrdersAsyncTaskCompletion) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i8* @_ZNSt5async12launch_asyncEv(i32, i8* (i8*)*, i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @worker(i8* %unused) {
    entry:
      store i32 77, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %future = call i8* @_ZNSt5async12launch_asyncEv(
          i32 1, i8* (i8*)* @worker, i8* null)
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("main"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       DetachedTaskCompletionOrdersDetachedTaskBeforeFollowerTask) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)

    @shared = global i32 0

    define internal void @detached_body() {
    entry:
      store i32 17, i32* @shared
      ret void
    }

    define internal void @follower_body() {
    entry:
      %v = load i32, i32* @shared
      ret void
    }

    define i32 @main() {
    entry:
      %detached = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 65, i64 32, i64 0, void ()* @detached_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %detached)
      call void @__kmpc_omp_task_complete_if0(i8* null, i32 0, i8* %detached)
      call i32 @__kmpc_omp_task(i8* null, i32 0,
          i8* bitcast (void ()* @follower_body to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("detached_body")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("follower_body"), "v");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest, PromiseFutureRepeatedQueriesRemainStable) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @promise_obj = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @_ZNSt7promise10get_futureEv(i8*)
    declare void @_ZNSt7promise9set_valueEv(i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 99, i32* @shared, align 4
      call void @_ZNSt7promise9set_valueEv(i8* @promise_obj)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      %future = call i8* @_ZNSt7promise10get_futureEv(i8* @promise_obj)
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
  EXPECT_FALSE(hb.happensBefore(load_shared, store_shared));
}
TEST_F(HappensBeforeAnalysisTest, AmbiguousPromiseFutureHandleDoesNotInventHB) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @promise1 = global i8 0
    @promise2 = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @_ZNSt7promise10get_futureEv(i8*)
    declare void @_ZNSt7promise9set_valueEv(i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @producer1(i8* %unused) {
    entry:
      store i32 11, i32* @shared, align 4
      call void @_ZNSt7promise9set_valueEv(i8* @promise1)
      ret i8* null
    }

    define i8* @producer2(i8* %unused) {
    entry:
      call void @_ZNSt7promise9set_valueEv(i8* @promise2)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      %future1 = call i8* @_ZNSt7promise10get_futureEv(i8* @promise1)
      %future2 = call i8* @_ZNSt7promise10get_futureEv(i8* @promise2)
      %future = select i1 true, i8* %future1, i8* %future2
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @producer2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer1 = module->getFunction("producer1");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer1, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer1->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest, LatchWaitSynchronizesAfterCountdown) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @latch = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @fake_latch_count_down(i8*)
    declare void @fake_latch_waitEv(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 5, i32* @shared, align 4
      call void @fake_latch_count_down(i8* @latch)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      call void @fake_latch_waitEv(i8* @latch)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
