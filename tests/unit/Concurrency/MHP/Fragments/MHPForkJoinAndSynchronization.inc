TEST_F(MHPAnalysisTest, ForkJoinOrdering) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
      %w1 = add i32 40, 2
      %w2 = add i32 %w1, 1
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %pre = add i32 1, 2
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %mid = add i32 3, 4
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *pre = findInstructionByName(*main_func, "pre");
  const Instruction *mid = findInstructionByName(*main_func, "mid");
  const Instruction *post = findInstructionByName(*main_func, "post");
  const Instruction *w1 = findInstructionByName(*worker_func, "w1");
  ASSERT_NE(pre, nullptr);
  ASSERT_NE(mid, nullptr);
  ASSERT_NE(post, nullptr);
  ASSERT_NE(w1, nullptr);

  EXPECT_TRUE(mhp.mustBeSequential(pre, w1));
  EXPECT_TRUE(mhp.mayHappenInParallel(mid, w1));
  EXPECT_TRUE(mhp.mustBeSequential(post, w1));
}
TEST_F(MHPAnalysisTest, ReusedThreadHandleStorageKeepsJoinAmbiguous) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      br i1 %cond, label %left, label %right

    left:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker1, i8* null)
      br label %join

    right:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker2, i8* null)
      br label %join

    join:
      call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *w1 =
      findInstructionByName(*module->getFunction("worker1"), "w1");
  const Instruction *w2 =
      findInstructionByName(*module->getFunction("worker2"), "w2");
  const Instruction *post =
      findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(w1, post));
  EXPECT_TRUE(mhp.mayHappenInParallel(w2, post));
}
TEST_F(MHPAnalysisTest, LoopForkDoesNotAutoSelfParallelizeWorkerBody) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
      %w1 = add i32 10, 20
      %w2 = add i32 %w1, 1
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      br label %loop

    loop:
      %i = phi i32 [0, %entry], [%inc, %loop]
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %inc = add i32 %i, 1
      %cond = icmp slt i32 %inc, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(worker_func, nullptr);

  const Instruction *w1 = findInstructionByName(*worker_func, "w1");
  const Instruction *w2 = findInstructionByName(*worker_func, "w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(w1, w1));
  EXPECT_TRUE(mhp.mayHappenInParallel(w1, w2));
}
TEST_F(MHPAnalysisTest, MutexSerializesCriticalSections) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define i8* @worker(i8* %arg) {
      %wl = call i32 @pthread_mutex_lock(i8* @lock)
      %w_in = add i32 7, 8
      %wu = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %ml = call i32 @pthread_mutex_lock(i8* @lock)
      %m_in = add i32 1, 2
      %mu = call i32 @pthread_mutex_unlock(i8* @lock)
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *m_in = findInstructionByName(*main_func, "m_in");
  const Instruction *w_in = findInstructionByName(*worker_func, "w_in");
  ASSERT_NE(m_in, nullptr);
  ASSERT_NE(w_in, nullptr);

  // Pure MHP tracks overlap only; the common mutex is a separate exclusion.
  auto *lockset = mhp.getLockSetAnalysis();
  ASSERT_NE(lockset, nullptr);
  auto stats = lockset->getStatistics();
  EXPECT_EQ(stats.num_locks, 1u);
  EXPECT_GE(stats.num_acquires, 2u);
  EXPECT_GE(stats.num_releases, 2u);
  EXPECT_TRUE(mhp.mayHappenInParallel(m_in, w_in));
}
TEST_F(MHPAnalysisTest, BarrierOrdersPreAndPostRegions) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_barrier_wait(i8*)

    @bar = global i8 0
    @shared = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      %bw = call i32 @pthread_barrier_wait(i8* @bar)
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %bw = call i32 @pthread_barrier_wait(i8* @bar)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_shared = &writer_func->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader_func, "val");
  ASSERT_NE(load_shared, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(hb.mustPrecede(store_shared, load_shared));
  EXPECT_FALSE(mhp.mayHappenInParallel(store_shared, load_shared));
}
TEST_F(MHPAnalysisTest, SplitPhaseBarrierOrdersPreAndPostRegions) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    @barrier = global i8 0
    @shared = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      store i32 17, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(hb.mustPrecede(store_shared, load_shared));
  EXPECT_FALSE(mhp.mayHappenInParallel(store_shared, load_shared));
}
TEST_F(MHPAnalysisTest, SplitPhaseBarrierArriveDoesNotOrderPostArriveCode) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    @barrier = global i8 0
    @lhs = global i32 0
    @rhs = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      call void @std_barrier_arriveEv(i8* @barrier)
      %after_arrive = load i32, i32* @lhs, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %before_wait = load i32, i32* @rhs, align 4
      call void @std_barrier_waitEv(i8* @barrier)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Instruction *after_arrive =
      findInstructionByName(*module->getFunction("writer"), "after_arrive");
  const Instruction *before_wait =
      findInstructionByName(*module->getFunction("reader"), "before_wait");
  ASSERT_NE(after_arrive, nullptr);
  ASSERT_NE(before_wait, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(mhp.mayHappenInParallel(after_arrive, before_wait));
  EXPECT_FALSE(hb.mustPrecede(after_arrive, before_wait));
  EXPECT_FALSE(hb.mustPrecede(before_wait, after_arrive));
}
TEST_F(MHPAnalysisTest, IncompleteBarrierPhaseDoesNotOrderPostBarrierCode) {
  const char *source = R"(
    @bar = global i8 0
    @shared = global i32 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_barrier_init(i8*, i8*, i32)
    declare i32 @pthread_barrier_wait(i8*)

    define i8* @writer(i8* %arg) {
    entry:
      call i32 @pthread_barrier_wait(i8* @bar)
      %writer_store = add i32 1, 2
      store i32 %writer_store, i32* @shared, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      call i32 @pthread_barrier_wait(i8* @bar)
      %reader_load = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_barrier_init(i8* @bar, i8* null, i32 3)
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *writer_store =
      findInstructionByName(*module->getFunction("writer"), "writer_store");
  const Instruction *reader_load =
      findInstructionByName(*module->getFunction("reader"), "reader_load");
  ASSERT_NE(writer_store, nullptr);
  ASSERT_NE(reader_load, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(writer_store, reader_load));
  EXPECT_FALSE(hb.mustPrecede(writer_store, reader_load));
}
TEST_F(MHPAnalysisTest, CondSignalDoesNotCreateDefiniteHBToAllWaiters) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)
    declare i32 @pthread_cond_signal(i8*)

    @cond = global i8 0
    @mutex = global i8 0
    @shared = global i32 0

    define i8* @waiter1(i8* %arg) {
    entry:
      %w1 = call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %load1 = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @waiter2(i8* %arg) {
    entry:
      %w2 = call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %load2 = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @signaler(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      %sig = call i32 @pthread_cond_signal(i8* @cond)
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

  const Function *signaler_func = module->getFunction("signaler");
  const Function *waiter1_func = module->getFunction("waiter1");
  const Function *waiter2_func = module->getFunction("waiter2");
  ASSERT_NE(signaler_func, nullptr);
  ASSERT_NE(waiter1_func, nullptr);
  ASSERT_NE(waiter2_func, nullptr);

  const Instruction *store_shared = &signaler_func->getEntryBlock().front();
  const Instruction *load1 = findInstructionByName(*waiter1_func, "load1");
  const Instruction *load2 = findInstructionByName(*waiter2_func, "load2");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load1, nullptr);
  ASSERT_NE(load2, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_FALSE(hb.mustPrecede(store_shared, load1));
  EXPECT_FALSE(hb.mustPrecede(store_shared, load2));
}
