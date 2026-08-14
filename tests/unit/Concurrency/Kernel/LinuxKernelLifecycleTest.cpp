#include "LinuxKernelAnalysisTestSupport.h"

TEST_F(LinuxKernelAnalysisTest,
       RCUBarrierJoinsCallbacksBeforeOwnerReclamation) {
  const char *source = R"(
    %owner = type { i8, i32 }

    declare i8* @kmalloc(i64)
    declare void @kfree(i8*)
    declare void @call_rcu(i8*, void ()*)
    declare void @rcu_barrier()

    define void @callback() { ret void }

    define void @unsafe_reclaim() {
    entry:
      %raw = call i8* @kmalloc(i64 16)
      %typed = bitcast i8* %raw to %owner*
      %head = getelementptr %owner, %owner* %typed, i32 0, i32 0
      call void @call_rcu(i8* %head, void ()* @callback)
      call void @kfree(i8* %raw)
      ret void
    }

    define void @safe_reclaim() {
    entry:
      %raw = call i8* @kmalloc(i64 16)
      %typed = bitcast i8* %raw to %owner*
      %head = getelementptr %owner, %owner* %typed, i32 0, i32 0
      call void @call_rcu(i8* %head, void ()* @callback)
      call void @rcu_barrier()
      call void @kfree(i8* %raw)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(OperationKind::RCU_BARRIER), 1U);
  ASSERT_EQ(analysis.getResults().async_lifetime_hazards.size(), 1U);
  EXPECT_EQ(analysis.getResults()
                .async_lifetime_hazards.front()
                .first->getFunction()
                ->getName(),
            "unsafe_reclaim");
}

TEST_F(LinuxKernelAnalysisTest, ModelsTaskletNapiAndSoftirqExecutionContexts) {
  const char *source = R"(
    @tasklet = global i8 0
    @napi = global i8 0
    @device = global i8 0

    define void @tasklet_fn() { ret void }
    define void @napi_fn() { ret void }
    define void @softirq_fn() { ret void }

    declare void @tasklet_setup(i8*, void ()*)
    declare void @tasklet_schedule(i8*)
    declare void @tasklet_kill(i8*)
    declare void @netif_napi_add(i8*, i8*, void ()*)
    declare void @napi_schedule(i8*)
    declare void @napi_disable(i8*)
    declare void @open_softirq(i32, void ()*)
    declare void @raise_softirq(i32)

    define void @register_async_contexts() {
    entry:
      call void @tasklet_setup(i8* @tasklet, void ()* @tasklet_fn)
      call void @tasklet_schedule(i8* @tasklet)
      call void @tasklet_kill(i8* @tasklet)
      call void @netif_napi_add(i8* @device, i8* @napi, void ()* @napi_fn)
      call void @napi_schedule(i8* @napi)
      call void @napi_disable(i8* @napi)
      call void @open_softirq(i32 3, void ()* @softirq_fn)
      call void @raise_softirq(i32 3)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(OperationKind::TASKLET_SCHEDULE), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::NAPI_SCHEDULE), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::SOFTIRQ_RAISE), 1U);

  std::set<AsyncContextKind> explicit_contexts;
  for (const auto &context : analysis.getExecutionGraph().getContexts()) {
    if (context.explicit_concurrency) {
      explicit_contexts.insert(context.kind);
    }
  }
  EXPECT_TRUE(explicit_contexts.count(AsyncContextKind::TASKLET));
  EXPECT_TRUE(explicit_contexts.count(AsyncContextKind::NAPI));
  EXPECT_TRUE(explicit_contexts.count(AsyncContextKind::SOFTIRQ));

  size_t synchronous_lifecycle_edges = 0;
  for (const auto &edge : analysis.getExecutionGraph().getEdges()) {
    synchronous_lifecycle_edges += edge.synchronous;
  }
  EXPECT_GE(synchronous_lifecycle_edges, 2U);
}

TEST_F(LinuxKernelAnalysisTest,
       DetectsPredicateCheckBeforeWaitQueueEnrollment) {
  const char *source = R"(
    @queue = global i8 0
    @entry = global i8 0
    @ready = global i1 false

    declare void @prepare_to_wait(i8*, i8*, i32)
    declare void @wake_up(i8*)
    declare i8* @kthread_run(void ()*, i8*)

    define void @bad_waiter() {
    entry:
      %ready_now = load i1, i1* @ready
      br i1 %ready_now, label %exit, label %enroll
    enroll:
      call void @prepare_to_wait(i8* @queue, i8* @entry, i32 0)
      ret void
    exit:
      ret void
    }

    define void @good_waiter() {
    entry:
      call void @prepare_to_wait(i8* @queue, i8* @entry, i32 0)
      %ready_now = load i1, i1* @ready
      br i1 %ready_now, label %exit, label %sleep
    sleep:
      ret void
    exit:
      ret void
    }

    define void @producer() {
    entry:
      store i1 true, i1* @ready
      call void @wake_up(i8* @queue)
      ret void
    }

    define void @start_wait_test() {
    entry:
      %bad = call i8* @kthread_run(void ()* @bad_waiter, i8* null)
      %good = call i8* @kthread_run(void ()* @good_waiter, i8* null)
      %prod = call i8* @kthread_run(void ()* @producer, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().lost_wakeups.size(), 1U);
  EXPECT_EQ(analysis.getResults()
                .lost_wakeups.front()
                .first->getFunction()
                ->getName(),
            "bad_waiter");
  EXPECT_EQ(analysis.getResults()
                .lost_wakeups.front()
                .second->getFunction()
                ->getName(),
            "producer");
}

TEST_F(LinuxKernelAnalysisTest,
       AppliesLocalIRQMaskingToUniprocessorContextFeasibility) {
  const char *source = R"(
    @shared = global i32 0

    declare i32 @request_irq(i32, void ()*, i64, i8*, i8*)
    declare void @local_irq_disable()
    declare void @local_irq_enable()

    define void @irq_handler() {
    entry:
      store i32 1, i32* @shared
      ret void
    }

    define void @register_and_update() {
    entry:
      %registered = call i32 @request_irq(i32 4, void ()* @irq_handler,
                                          i64 0, i8* null, i8* null)
      call void @local_irq_disable()
      store i32 2, i32* @shared
      call void @local_irq_enable()
      ret void
    }
  )";

  auto smp_module = parseModule(source);
  ASSERT_NE(smp_module, nullptr);
  LinuxKernelConfig smp_config;
  smp_config.smp = true;
  LinuxKernelAnalysis smp(*smp_module, smp_config);
  smp.runAnalysis();
  EXPECT_EQ(smp.getResults().data_races.size(), 1U);

  auto up_module = parseModule(source);
  ASSERT_NE(up_module, nullptr);
  LinuxKernelConfig up_config;
  up_config.smp = false;
  LinuxKernelAnalysis up(*up_module, up_config);
  up.runAnalysis();
  EXPECT_TRUE(up.getResults().data_races.empty());
}

TEST_F(LinuxKernelAnalysisTest, ModelsDeferredRcuReclamationWithoutCallback) {
  const char *source = R"(
    @object = global i8 0

    declare void @kfree_rcu(i8*, i64)

    define void @defer_free() {
    entry:
      call void @kfree_rcu(i8* @object, i64 16)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto operations =
      analysis.getProcessModel().getOperationsByKind(OperationKind::RCU_CALL);
  ASSERT_EQ(operations.size(), 1U);
  EXPECT_TRUE(operations.front().deferred_reclamation);
  EXPECT_EQ(operations.front().rcu_target, module->getNamedGlobal("object"));
  EXPECT_EQ(operations.front().callback, nullptr);
  EXPECT_TRUE(operations.front().async_callbacks.empty());
  EXPECT_TRUE(analysis.getExecutionGraph().getContexts().empty());
}

TEST_F(LinuxKernelAnalysisTest, UsesApiSpecificAllocationSizeOperands) {
  const char *source = R"(
    declare i8* @devm_kmalloc(i8*, i64, i32)
    declare i8* @kmem_cache_alloc(i8*, i32)
    declare i8* @kmalloc_array(i64, i64, i32)

    define void @allocate(i8* %device, i8* %cache) {
    entry:
      %managed = call i8* @devm_kmalloc(i8* %device, i64 64, i32 0)
      %slab = call i8* @kmem_cache_alloc(i8* %cache, i32 0)
      %array = call i8* @kmalloc_array(i64 8, i64 16, i32 0)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto allocations =
      analysis.getProcessModel().getOperationsByKind(OperationKind::KMALLOC);
  ASSERT_EQ(allocations.size(), 3U);
  const KernelOperation *managed = nullptr;
  const KernelOperation *slab = nullptr;
  const KernelOperation *array = nullptr;
  for (const KernelOperation &allocation : allocations) {
    if (allocation.function_name == "devm_kmalloc") {
      managed = &allocation;
    } else if (allocation.function_name == "kmem_cache_alloc") {
      slab = &allocation;
    } else if (allocation.function_name == "kmalloc_array") {
      array = &allocation;
    }
  }
  ASSERT_NE(managed, nullptr);
  ASSERT_NE(slab, nullptr);
  ASSERT_NE(array, nullptr);
  const auto *size = dyn_cast<ConstantInt>(managed->allocation_size);
  ASSERT_NE(size, nullptr);
  EXPECT_EQ(size->getZExtValue(), 64U);
  EXPECT_TRUE(managed->managed_allocation);
  EXPECT_EQ(slab->allocation_size, nullptr);
  EXPECT_EQ(array->allocation_size, nullptr);
}

TEST_F(LinuxKernelAnalysisTest,
       ExternalEntryConcurrencyIsExplicitlyConfigured) {
  const char *source = R"(
    @shared = global i32 0

    define void @entry_a() {
    entry:
      store i32 1, i32* @shared
      ret void
    }

    define void @entry_b() {
    entry:
      store i32 2, i32* @shared
      ret void
    }
  )";

  auto explicit_only_module = parseModule(source);
  ASSERT_NE(explicit_only_module, nullptr);
  LinuxKernelAnalysis explicit_only(*explicit_only_module);
  explicit_only.runAnalysis();
  EXPECT_TRUE(explicit_only.getResults().data_races.empty());

  auto external_module = parseModule(source);
  ASSERT_NE(external_module, nullptr);
  LinuxKernelConfig config;
  config.assume_external_entries_parallel = true;
  LinuxKernelAnalysis external(*external_module, config);
  external.runAnalysis();
  ASSERT_EQ(external.getResults().data_races.size(), 1U);
  EXPECT_NE(external.getResults().data_races.front().first,
            external.getResults().data_races.front().second);
}

TEST_F(LinuxKernelAnalysisTest, LoadsProjectSpecificKernelApiSemantics) {
  SmallString<128> spec_path;
  int spec_fd = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-kernel-api", "spec", spec_fd,
                                            spec_path));
  {
    raw_fd_ostream spec(spec_fd, true);
    spec << "vendor_try_lock LOCK_TRY lock=mutex mode=exclusive object=1 "
            "success=nonzero may-sleep=true\n";
    spec << "vendor_submit WORKqueue_SUBMIT object=0 callback=1 "
            "context=workqueue may-spawn=true\n";
  }

  const char *source = R"(
    @lock = global i8 0
    @work = global i8 0

    declare i32 @vendor_try_lock(i32, i8*)
    declare void @vendor_submit(i8*, void ()*)

    define void @worker() {
    entry:
      ret void
    }

    define void @use_vendor_api() {
    entry:
      %locked = call i32 @vendor_try_lock(i32 7, i8* @lock)
      call void @vendor_submit(i8* @work, void ()* @worker)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelConfig config;
  config.load_default_api_specs = false;
  config.require_api_specs = true;
  config.api_spec_paths.push_back(spec_path.str().str());
  LinuxKernelAnalysis analysis(*module, config);
  analysis.runAnalysis();

  EXPECT_TRUE(
      analysis.getProcessModel().getSemanticRegistry().getErrors().empty());
  ASSERT_EQ(
      analysis.getProcessModel().getSemanticRegistry().getLoadedFiles().size(),
      1U);

  const auto locks =
      analysis.getProcessModel().getOperationsByKind(OperationKind::LOCK_TRY);
  ASSERT_EQ(locks.size(), 1U);
  EXPECT_EQ(locks.front().lock, module->getNamedGlobal("lock"));
  EXPECT_EQ(locks.front().lock_kind, LockKind::MUTEX);
  EXPECT_EQ(locks.front().conditional_success, ConditionalSuccess::NONZERO);
  EXPECT_TRUE(locks.front().may_sleep);

  const auto submissions = analysis.getProcessModel().getOperationsByKind(
      OperationKind::WORKqueue_SUBMIT);
  ASSERT_EQ(submissions.size(), 1U);
  EXPECT_EQ(submissions.front().async_object, module->getNamedGlobal("work"));
  EXPECT_EQ(submissions.front().callback->getName(), "worker");
  EXPECT_EQ(submissions.front().async_context, AsyncContextKind::WORKQUEUE);
  EXPECT_TRUE(submissions.front().may_spawn);

  EXPECT_FALSE(sys::fs::remove(spec_path));
}

TEST_F(LinuxKernelAnalysisTest, DisabledSpecsDoNotUseHardcodedSemantics) {
  const char *source = R"(
    @lock = global i8 0

    declare void @spin_lock(i8*)

    define void @take_lock() {
    entry:
      call void @spin_lock(i8* @lock)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelConfig config;
  config.load_default_api_specs = false;
  config.require_api_specs = false;
  LinuxKernelAnalysis analysis(*module, config);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(OperationKind::LOCK_ACQUIRE), 0U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::UNKNOWN_CALL), 1U);
  EXPECT_TRUE(analysis.getProcessModel()
                  .getSemanticRegistry()
                  .getLoadedFiles()
                  .empty());
}

TEST_F(LinuxKernelAnalysisTest, MalformedSpecsFailClosedEvenWhenOptional) {
  SmallString<128> spec_path;
  int spec_fd = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-kernel-invalid-api", "spec",
                                            spec_fd, spec_path));
  {
    raw_fd_ostream spec(spec_fd, true);
    spec << "spin_lock LOCK_ACQUIRE lock=spin object=0\n";
    spec << "spin_unlock LOCK_RELEASE unsupported=true\n";
  }

  const char *source = R"(
    @lock = global i8 0

    declare void @spin_lock(i8*)

    define void @take_lock() {
    entry:
      call void @spin_lock(i8* @lock)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelConfig config;
  config.load_default_api_specs = false;
  config.require_api_specs = false;
  config.api_spec_paths.push_back(spec_path.str().str());
  LinuxKernelAnalysis analysis(*module, config);
  analysis.runAnalysis();

  EXPECT_FALSE(
      analysis.getProcessModel().getSemanticRegistry().getErrors().empty());
  EXPECT_TRUE(analysis.getProcessModel()
                  .getSemanticRegistry()
                  .getLoadedFiles()
                  .empty());
  EXPECT_EQ(analysis.getOperationCount(OperationKind::LOCK_ACQUIRE), 0U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::UNKNOWN_CALL), 1U);

  EXPECT_FALSE(sys::fs::remove(spec_path));
}
