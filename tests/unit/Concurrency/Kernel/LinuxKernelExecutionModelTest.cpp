#include "LinuxKernelAnalysisTestSupport.h"

TEST_F(LinuxKernelAnalysisTest,
       FindsConcurrentConditionProducerThatOmitsWakeUp) {
  const char *source = R"(
    @bad_queue = global i8 0
    @good_queue = global i8 0
    @bad_ready = global i1 false
    @good_ready = global i1 false

    declare void @wait_event(i8*, i1)
    declare void @wake_up(i8*)
    declare i8* @kthread_run(void ()*, i8*)

    define void @bad_consumer() {
    entry:
      %ready = load i1, i1* @bad_ready
      call void @wait_event(i8* @bad_queue, i1 %ready)
      ret void
    }

    define void @bad_producer() {
    entry:
      store i1 true, i1* @bad_ready
      ret void
    }

    define void @good_consumer() {
    entry:
      %ready = load i1, i1* @good_ready
      call void @wait_event(i8* @good_queue, i1 %ready)
      ret void
    }

    define void @good_producer() {
    entry:
      store i1 true, i1* @good_ready
      call void @wake_up(i8* @good_queue)
      ret void
    }

    define void @start_waiters() {
    entry:
      %bc = call i8* @kthread_run(void ()* @bad_consumer, i8* null)
      %bp = call i8* @kthread_run(void ()* @bad_producer, i8* null)
      %gc = call i8* @kthread_run(void ()* @good_consumer, i8* null)
      %gp = call i8* @kthread_run(void ()* @good_producer, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &missing = analysis.getResults().missing_wake_ups;
  ASSERT_EQ(missing.size(), 1U);
  EXPECT_EQ(missing.front().first->getFunction()->getName(), "bad_consumer");
  EXPECT_EQ(missing.front().second->getFunction()->getName(), "bad_producer");
}

TEST_F(LinuxKernelAnalysisTest,
       ExtractsKernelMemoryEventsAndFiltersLockProtectedAccesses) {
  const char *source = R"(
    @racy = global i32 0
    @safe = global i32 0
    @marked = global i32 0
    @guard = global i8 0

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)
    declare i8* @kthread_run(void ()*, i8*)
    declare void @smp_mb()

    define void @racy_writer() {
    entry:
      store i32 1, i32* @racy
      ret void
    }

    define void @racy_reader() {
    entry:
      %value = load i32, i32* @racy
      ret void
    }

    define void @safe_writer() {
    entry:
      call void @spin_lock(i8* @guard)
      store i32 1, i32* @safe
      call void @spin_unlock(i8* @guard)
      ret void
    }

    define void @safe_reader() {
    entry:
      call void @spin_lock(i8* @guard)
      %value = load i32, i32* @safe
      call void @spin_unlock(i8* @guard)
      ret void
    }

    define void @marked_writer() {
    entry:
      store volatile i32 1, i32* @marked
      call void @smp_mb()
      ret void
    }

    define void @marked_reader() {
    entry:
      %value = load volatile i32, i32* @marked
      ret void
    }

    define void @start_memory_workers() {
    entry:
      %rw = call i8* @kthread_run(void ()* @racy_writer, i8* null)
      %rr = call i8* @kthread_run(void ()* @racy_reader, i8* null)
      %sw = call i8* @kthread_run(void ()* @safe_writer, i8* null)
      %sr = call i8* @kthread_run(void ()* @safe_reader, i8* null)
      %mw = call i8* @kthread_run(void ()* @marked_writer, i8* null)
      %mr = call i8* @kthread_run(void ()* @marked_reader, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().data_races.size(), 1U);
  const auto &race = analysis.getResults().data_races.front();
  std::set<StringRef> functions = {race.first->getFunction()->getName(),
                                   race.second->getFunction()->getName()};
  EXPECT_EQ(functions, (std::set<StringRef>{"racy_reader", "racy_writer"}));

  bool saw_full_fence = false;
  bool saw_marked_access = false;
  for (const auto &event : analysis.getMemoryModel().getEvents()) {
    saw_full_fence |= event.kind == LinuxKernelMemoryModel::EventKind::FENCE &&
                      event.order == LinuxKernelMemoryModel::MemoryOrder::FULL;
    saw_marked_access |=
        event.kind == LinuxKernelMemoryModel::EventKind::MARKED_READ ||
        event.kind == LinuxKernelMemoryModel::EventKind::MARKED_WRITE;
  }
  EXPECT_TRUE(saw_full_fence);
  EXPECT_TRUE(saw_marked_access);

  auto diagnostic = std::find_if(
      analysis.getResults().diagnostics.begin(),
      analysis.getResults().diagnostics.end(), [](const auto &finding) {
        return finding.category == "kernel-data-race";
      });
  ASSERT_NE(diagnostic, analysis.getResults().diagnostics.end());
  EXPECT_FALSE(diagnostic->stable_id.empty());
  EXPECT_EQ(diagnostic->witness.size(), 2U);
  EXPECT_FALSE(diagnostic->assumptions.empty());
}

TEST_F(LinuxKernelAnalysisTest, ReportsExplicitKernelConfigurationAssumptions) {
  auto module = parseModule("define void @empty() { ret void }");
  ASSERT_NE(module, nullptr);

  LinuxKernelConfig config;
  config.kernel_version = "6.test";
  config.architecture = "arm64";
  config.preemption = KernelPreemptionModel::RT;
  config.smp = false;
  LinuxKernelAnalysis analysis(*module, config);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getConfig().isPreemptRT());
  EXPECT_FALSE(analysis.getProcessModel()
                   .getSemanticRegistry()
                   .getLoadedFiles()
                   .empty());
  EXPECT_TRUE(
      analysis.getProcessModel().getSemanticRegistry().getErrors().empty());
  std::string output;
  raw_string_ostream stream(output);
  analysis.printResults(stream);
  stream.flush();
  EXPECT_NE(output.find("arch=arm64"), std::string::npos);
  EXPECT_NE(output.find("smp=off"), std::string::npos);
  EXPECT_NE(output.find("preemption=rt"), std::string::npos);
  EXPECT_NE(output.find("kernel=6.test"), std::string::npos);
}

TEST_F(LinuxKernelAnalysisTest,
       ModelsKthreadStartAndSynchronousStopLifecycleOrdering) {
  const char *source = R"(
    @shared = global i32 0

    declare i8* @kthread_create(void ()*, i8*)
    declare i32 @wake_up_process(i8*)
    declare i32 @kthread_stop(i8*)

    define void @thread_fn() {
    entry:
      store i32 1, i32* @shared
      ret void
    }

    define void @manage_thread() {
    entry:
      %task = call i8* @kthread_create(void ()* @thread_fn, i8* null)
      %started = call i32 @wake_up_process(i8* %task)
      store i32 2, i32* @shared
      %stopped = call i32 @kthread_stop(i8* %task)
      store i32 3, i32* @shared
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(OperationKind::KTHREAD_CREATE), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::KTHREAD_START), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::KTHREAD_STOP), 1U);
  ASSERT_EQ(analysis.getResults().data_races.size(), 1U);

  const auto &race = analysis.getResults().data_races.front();
  const Instruction *manager_access =
      race.first->getFunction()->getName() == "manage_thread" ? race.first
                                                              : race.second;
  const auto *store = dyn_cast<StoreInst>(manager_access);
  ASSERT_NE(store, nullptr);
  const auto *value = dyn_cast<ConstantInt>(store->getValueOperand());
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->getZExtValue(), 2U);
}

TEST_F(LinuxKernelAnalysisTest, PreservesUnresolvedCallsAsDeferredEffects) {
  const char *source = R"(
    @object = global i8 0
    declare void @driver_specific_helper(i8*)

    define void @entry() {
    entry:
      call void @driver_specific_helper(i8* @object)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().unresolved_calls.size(), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::UNKNOWN_CALL), 1U);
  auto finding = std::find_if(
      analysis.getResults().diagnostics.begin(),
      analysis.getResults().diagnostics.end(), [](const auto &diagnostic) {
        return diagnostic.category == "kernel-unresolved-call";
      });
  ASSERT_NE(finding, analysis.getResults().diagnostics.end());
  EXPECT_EQ(finding->confidence,
            LinuxKernelAnalysis::FindingConfidence::DEFERRED);
  EXPECT_FALSE(finding->unresolved.empty());

  std::string sarif;
  raw_string_ostream sarif_stream(sarif);
  analysis.printSARIF(sarif_stream);
  sarif_stream.flush();
  EXPECT_NE(sarif.find("\"version\":\"2.1.0\""), std::string::npos);
  EXPECT_NE(sarif.find("kernel-unresolved-call"), std::string::npos);
  EXPECT_NE(sarif.find("lotusStableId"), std::string::npos);
  EXPECT_NE(sarif.find("callee set or semantic summary is unresolved"),
            std::string::npos);
  auto parsed_sarif = llvm::json::parse(sarif);
  EXPECT_TRUE(static_cast<bool>(parsed_sarif));
  if (!parsed_sarif) {
    llvm::consumeError(parsed_sarif.takeError());
  }
}

TEST_F(LinuxKernelAnalysisTest,
       DetectsDirectAndAsynchronousOwnerLifetimeHazards) {
  const char *source = R"(
    %owner = type { i8, i32 }

    declare i8* @kmalloc(i64)
    declare void @kfree(i8*)
    declare void @timer_setup(i8*, void ()*, i32)
    declare void @mod_timer(i8*, i64)
    declare i32 @del_timer_sync(i8*)

    define void @timer_fn() { ret void }

    define void @unsafe_timer_owner() {
    entry:
      %raw = call i8* @kmalloc(i64 16)
      %typed = bitcast i8* %raw to %owner*
      %timer = getelementptr %owner, %owner* %typed, i32 0, i32 0
      call void @timer_setup(i8* %timer, void ()* @timer_fn, i32 0)
      call void @mod_timer(i8* %timer, i64 10)
      call void @kfree(i8* %raw)
      ret void
    }

    define void @safe_timer_owner() {
    entry:
      %raw = call i8* @kmalloc(i64 16)
      %typed = bitcast i8* %raw to %owner*
      %timer = getelementptr %owner, %owner* %typed, i32 0, i32 0
      call void @timer_setup(i8* %timer, void ()* @timer_fn, i32 0)
      call void @mod_timer(i8* %timer, i64 10)
      %cancelled = call i32 @del_timer_sync(i8* %timer)
      call void @kfree(i8* %raw)
      ret void
    }

    define void @direct_uaf() {
    entry:
      %raw = call i8* @kmalloc(i64 8)
      call void @kfree(i8* %raw)
      store i8 1, i8* %raw
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().use_after_free.size(), 1U);
  EXPECT_EQ(
      analysis.getResults().use_after_free.front()->getFunction()->getName(),
      "direct_uaf");
  ASSERT_EQ(analysis.getResults().async_lifetime_hazards.size(), 1U);
  EXPECT_EQ(analysis.getResults()
                .async_lifetime_hazards.front()
                .first->getFunction()
                ->getName(),
            "unsafe_timer_owner");
}

TEST_F(LinuxKernelAnalysisTest,
       FindsLockdepStyleStrongCyclesLongerThanTwoLocks) {
  const char *source = R"(
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)
    declare i8* @kthread_run(void ()*, i8*)

    define void @edge_ab() {
    entry:
      call void @spin_lock(i8* @a)
      call void @spin_lock(i8* @b)
      call void @spin_unlock(i8* @b)
      call void @spin_unlock(i8* @a)
      ret void
    }

    define void @edge_bc() {
    entry:
      call void @spin_lock(i8* @b)
      call void @spin_lock(i8* @c)
      call void @spin_unlock(i8* @c)
      call void @spin_unlock(i8* @b)
      ret void
    }

    define void @edge_ca() {
    entry:
      call void @spin_lock(i8* @c)
      call void @spin_lock(i8* @a)
      call void @spin_unlock(i8* @a)
      call void @spin_unlock(i8* @c)
      ret void
    }

    define void @start_cycle() {
    entry:
      %ab = call i8* @kthread_run(void ()* @edge_ab, i8* null)
      %bc = call i8* @kthread_run(void ()* @edge_bc, i8* null)
      %ca = call i8* @kthread_run(void ()* @edge_ca, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().lock_order_inversions.empty());
  ASSERT_EQ(analysis.getResults().lock_dependency_cycles.size(), 1U);
  EXPECT_EQ(
      analysis.getResults().lock_dependency_cycles.front().evidence.size(), 3U);
  EXPECT_EQ(analysis.getResults().lock_deadlocks.size(), 1U);

  auto finding = std::find_if(
      analysis.getResults().diagnostics.begin(),
      analysis.getResults().diagnostics.end(), [](const auto &diagnostic) {
        return diagnostic.category == "kernel-lock-strong-cycle";
      });
  ASSERT_NE(finding, analysis.getResults().diagnostics.end());
  EXPECT_EQ(finding->witness.size(), 3U);
}

TEST_F(LinuxKernelAnalysisTest, ExtractsLKMMAddressDataAndControlDependencies) {
  const char *source = R"(
    @value = global i32 0
    @pointer = global i32* @value
    @flag = global i32 0
    @data_out = global i32 0
    @control_out = global i32 0

    define void @dependencies() {
    entry:
      %ptr = load i32*, i32** @pointer
      %through_ptr = load i32, i32* %ptr
      %read_flag = load i32, i32* @flag
      %derived = add i32 %read_flag, 1
      store i32 %derived, i32* @data_out
      %condition = icmp ne i32 %read_flag, 0
      br i1 %condition, label %controlled, label %exit
    controlled:
      store i32 1, i32* @control_out
      br label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  std::set<LinuxKernelMemoryModel::RelationKind> dependency_kinds;
  for (const auto &relation : analysis.getMemoryModel().getRelations()) {
    if (relation.kind != LinuxKernelMemoryModel::RelationKind::PROGRAM_ORDER) {
      dependency_kinds.insert(relation.kind);
    }
  }
  EXPECT_TRUE(dependency_kinds.count(
      LinuxKernelMemoryModel::RelationKind::ADDRESS_DEPENDENCY));
  EXPECT_TRUE(dependency_kinds.count(
      LinuxKernelMemoryModel::RelationKind::DATA_DEPENDENCY));
  EXPECT_TRUE(dependency_kinds.count(
      LinuxKernelMemoryModel::RelationKind::CONTROL_DEPENDENCY));
}

TEST_F(LinuxKernelAnalysisTest, HonorsOrderedWorkqueueSerializationDomains) {
  const char *source = R"(
    @ordered_first = global i8 0
    @ordered_second = global i8 0
    @unordered_first = global i8 0
    @unordered_second = global i8 0
    @ordered_shared = global i32 0
    @unordered_shared = global i32 0

    declare i8* @alloc_ordered_workqueue(i8*, i32)
    declare i8* @alloc_workqueue(i8*, i32)
    declare void @__init_work(i8*, void ()*)
    declare i1 @queue_work(i8*, i8*)

    define void @ordered_writer_one() {
      store i32 1, i32* @ordered_shared
      ret void
    }
    define void @ordered_writer_two() {
      store i32 2, i32* @ordered_shared
      ret void
    }
    define void @unordered_writer_one() {
      store i32 1, i32* @unordered_shared
      ret void
    }
    define void @unordered_writer_two() {
      store i32 2, i32* @unordered_shared
      ret void
    }

    define void @queue_all_work() {
    entry:
      %ordered = call i8* @alloc_ordered_workqueue(i8* null, i32 0)
      %unordered = call i8* @alloc_workqueue(i8* null, i32 0)
      call void @__init_work(i8* @ordered_first,
                             void ()* @ordered_writer_one)
      call void @__init_work(i8* @ordered_second,
                             void ()* @ordered_writer_two)
      call void @__init_work(i8* @unordered_first,
                             void ()* @unordered_writer_one)
      call void @__init_work(i8* @unordered_second,
                             void ()* @unordered_writer_two)
      %o1 = call i1 @queue_work(i8* %ordered, i8* @ordered_first)
      %o2 = call i1 @queue_work(i8* %ordered, i8* @ordered_second)
      %u1 = call i1 @queue_work(i8* %unordered, i8* @unordered_first)
      %u2 = call i1 @queue_work(i8* %unordered, i8* @unordered_second)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().data_races.size(), 1U);
  const auto &race = analysis.getResults().data_races.front();
  EXPECT_TRUE(race.first->getFunction()->getName().contains("unordered"));
  EXPECT_TRUE(race.second->getFunction()->getName().contains("unordered"));
}
