#include "HappensBeforeAnalysisTestSupport.h"

TEST_F(HappensBeforeAnalysisTest, LatchArriveAndWaitSynchronizesAfterArrival) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @latch = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @fake_latch_arrive_and_wait(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 13, i32* @shared, align 4
      call void @fake_latch_arrive_and_wait(i8* @latch)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      call void @fake_latch_arrive_and_wait(i8* @latch)
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
TEST_F(HappensBeforeAnalysisTest, BarrierWaitSynchronizesAfterArrival) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @fake_barrier_arrive_and_wait(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 8, i32* @shared, align 4
      call void @fake_barrier_arrive_and_wait(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @fake_barrier_arrive_and_wait(i8* @barrier)
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_shared = &writer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       IncompleteBarrierPhaseDoesNotSynchronizeAfterWait) {
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

  const Instruction *store_shared =
      findInstructionByName(*module->getFunction("writer"), "writer_store");
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "reader_load");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest, SplitPhaseBarrierArriveSynchronizesWithWait) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 11, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_shared = &writer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       ReusedSplitPhaseBarrierKeepsBarrierCyclesSeparated) {
  const char *source = R"(
    @first = global i32 0, align 4
    @second = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 1, i32* @first, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      call void @std_barrier_waitEv(i8* @barrier)
      store i32 2, i32* @second, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      call void @std_barrier_waitEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      %first_load = load i32, i32* @first, align 4
      call void @std_barrier_waitEv(i8* @barrier)
      %second_load = load i32, i32* @second, align 4
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_first = nullptr;
  const Instruction *store_second = nullptr;
  for (const Instruction &inst : instructions(*writer)) {
    const auto *store = dyn_cast<StoreInst>(&inst);
    if (!store) {
      continue;
    }
    if (store->getPointerOperand() == module->getNamedGlobal("first")) {
      store_first = &inst;
    } else if (store->getPointerOperand() == module->getNamedGlobal("second")) {
      store_second = &inst;
    }
  }
  const Instruction *first_load = findInstructionByName(*reader, "first_load");
  const Instruction *second_load =
      findInstructionByName(*reader, "second_load");
  ASSERT_NE(store_first, nullptr);
  ASSERT_NE(store_second, nullptr);
  ASSERT_NE(first_load, nullptr);
  ASSERT_NE(second_load, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_first, first_load));
  EXPECT_TRUE(hb.happensBefore(store_second, second_load));
  EXPECT_FALSE(hb.happensBefore(store_second, first_load));
}
TEST_F(HappensBeforeAnalysisTest,
       SplitPhaseBarrierWithMultipleParticipantsSynchronizes) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    define i8* @writer1(i8* %unused) {
    entry:
      store i32 21, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @writer2(i8* %unused) {
    entry:
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader1(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @reader2(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      %tid4 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @writer2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader1, i8* null)
      call i32 @pthread_create(i8* %tid4, i8* null, i8* (i8*)* @reader2, i8* null)
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
      &module->getFunction("writer1")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader1"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       RepeatedSplitPhaseBarrierWaitsStillOrderLaterContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 31, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       BarrierArriveAndWaitSynchronizesPostBarrierContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @fake_barrier_arrive_and_wait(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 41, i32* @shared, align 4
      call void @fake_barrier_arrive_and_wait(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @fake_barrier_arrive_and_wait(i8* @barrier)
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest, OpenMPTaskDependenciesContributeToHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps_out = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]
    @deps_in = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 1
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep_out = getelementptr inbounds [1 x %kmp_depend_info],
                 [1 x %kmp_depend_info]* @deps_out, i64 0, i64 0
      %dep_in = getelementptr inbounds [1 x %kmp_depend_info],
                [1 x %kmp_depend_info]* @deps_in, i64 0, i64 0
      %t1 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep_out, i32 0, %kmp_depend_info* null)
      %t2 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep_in, i32 0, %kmp_depend_info* null)
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
  ASSERT_NE(main_func, nullptr);
  const Instruction *t1 = findInstructionByName(*main_func, "t1");
  const Instruction *t2 = findInstructionByName(*main_func, "t2");
  ASSERT_NE(t1, nullptr);
  ASSERT_NE(t2, nullptr);

  EXPECT_TRUE(hb.happensBefore(t1, t2));
}
TEST_F(HappensBeforeAnalysisTest, OpenMPTaskBodyDependenciesContributeToHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps_out = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]
    @deps_in = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 1
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i8* @producer_task(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      ret i8* null
    }

    define i8* @consumer_task(i8* %arg) {
    entry:
      %loaded = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task1 = alloca i8* (i8*)*, align 8
      %task2 = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @producer_task, i8* (i8*)** %task1, align 8
      store i8* (i8*)* @consumer_task, i8* (i8*)** %task2, align 8
      %task1_raw = bitcast i8* (i8*)** %task1 to i8*
      %task2_raw = bitcast i8* (i8*)** %task2 to i8*
      %dep_out = getelementptr inbounds [1 x %kmp_depend_info],
                 [1 x %kmp_depend_info]* @deps_out, i64 0, i64 0
      %dep_in = getelementptr inbounds [1 x %kmp_depend_info],
                [1 x %kmp_depend_info]* @deps_in, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task1_raw, i32 1,
          %kmp_depend_info* %dep_out, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task2_raw, i32 1,
          %kmp_depend_info* %dep_in, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer_task");
  const Function *consumer = module->getFunction("consumer_task");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "loaded");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest, OpenMPSingleBoundaryOrdersTaskContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_single(i8*, i32)
    declare void @__kmpc_end_single(i8*, i32)

    define i8* @task_body(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @task_body, i8* (i8*)** %task, align 8
      %task_raw = bitcast i8* (i8*)** %task to i8*
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task_raw)
      %single = call i32 @__kmpc_single(i8* null, i32 0)
      call void @__kmpc_end_single(i8* null, i32 0)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *task_body = module->getFunction("task_body");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(task_body, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *task_store = &task_body->getEntryBlock().front();
  const Instruction *after = findInstructionByName(*main_func, "after");
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_TRUE(hb.happensBefore(task_store, after));
}
TEST_F(HappensBeforeAnalysisTest, OpenMPFlushRelationFeedsHBAnalysis) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_flush(i8*)

    define i8* @producer_task(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @consumer_task(i8* %arg) {
    entry:
      %loaded = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task1 = alloca i8* (i8*)*, align 8
      %task2 = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @producer_task, i8* (i8*)** %task1, align 8
      store i8* (i8*)* @consumer_task, i8* (i8*)** %task2, align 8
      %task1_raw = bitcast i8* (i8*)** %task1 to i8*
      %task2_raw = bitcast i8* (i8*)** %task2 to i8*
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
             [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task1_raw, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_flush(i8* bitcast (i32* @shared to i8*))
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task2_raw, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer_task");
  const Function *consumer = module->getFunction("consumer_task");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "loaded");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}
TEST_F(HappensBeforeAnalysisTest,
       PlainReleaseAcquireWithoutWitnessStaysDeferred) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %val = load i32, i32* @data, align 4
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
  EXPECT_FALSE(hb.happensBefore(load_data, store_data));
}
TEST_F(HappensBeforeAnalysisTest,
       DirectReleaseAcquireWithAssumeWitnessCreatesHBWithoutBranch) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @llvm.assume(i1)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 23, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      call void @llvm.assume(i1 %ready)
      %val = load i32, i32* @data, align 4
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest,
       ReleaseFenceAcquireFenceWithAssumeWitnessCreatesHBWithoutBranch) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @llvm.assume(i1)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 29, i32* @data, align 4
      fence release
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      call void @llvm.assume(i1 %ready)
      %val = load i32, i32* @data, align 4
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}
