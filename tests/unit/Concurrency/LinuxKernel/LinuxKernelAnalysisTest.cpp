#include "LinuxKernelAnalysisTestSupport.h"

TEST_F(LinuxKernelAnalysisTest, TracksExtendedKernelOperationKinds) {
  const char *source = R"(
    @timer = global i8 0, align 1
    @counter = global i32 0, align 4

    declare void @timer_setup(i8*, i8*, i32)
    declare void @mod_timer(i8*, i64)
    declare i32 @del_timer_sync(i8*)
    declare i32 @atomic_add_return(i32, i32*)
    declare void @local_irq_disable()
    declare void @local_irq_enable()

    define void @kernel_ops() {
    entry:
      call void @timer_setup(i8* @timer, i8* null, i32 0)
      call void @mod_timer(i8* @timer, i64 25)
      %deleted = call i32 @del_timer_sync(i8* @timer)
      %old = call i32 @atomic_add_return(i32 1, i32* @counter)
      call void @local_irq_disable()
      call void @local_irq_enable()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(OperationKind::TIMER_SETUP), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::TIMER_MOD), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::TIMER_DELETE), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::ATOMIC_RMW), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::IRQ_DISABLE), 1U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::IRQ_ENABLE), 1U);
}

TEST_F(LinuxKernelAnalysisTest, FindsLockOrderingAndSleepInAtomicContext) {
  const char *source = R"(
    @lock_a = global i8 0, align 1
    @lock_b = global i8 0, align 1
    @raw_lock = global i8 0, align 1
    @mutex_lock_obj = global i8 0, align 1

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)
    declare void @raw_spin_lock(i8*)
    declare void @mutex_lock(i8*)
    declare void @mutex_unlock(i8*)
    declare void @spin_lock_irqsave(i8*, i64*)

    define void @path_one() {
    entry:
      call void @spin_lock(i8* @lock_a)
      call void @spin_lock(i8* @lock_b)
      call void @spin_unlock(i8* @lock_b)
      call void @mutex_lock(i8* @mutex_lock_obj)
      call void @mutex_unlock(i8* @mutex_lock_obj)
      call void @spin_unlock(i8* @lock_a)
      ret void
    }

    define void @path_two() {
    entry:
      call void @spin_lock(i8* @lock_b)
      call void @spin_lock(i8* @lock_a)
      call void @spin_unlock(i8* @lock_a)
      call void @spin_unlock(i8* @lock_b)
      ret void
    }

    define void @mixed_locking() {
    entry:
      call void @raw_spin_lock(i8* @raw_lock)
      call void @spin_unlock(i8* @raw_lock)
      ret void
    }

    define void @irqsave_mismatch(i64* %flags) {
    entry:
      call void @spin_lock_irqsave(i8* @lock_a, i64* %flags)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &results = analysis.getResults();

  EXPECT_EQ(results.lock_order_inversions.size(), 1U);
  EXPECT_EQ(results.lock_deadlocks.size(), 0U);
  EXPECT_EQ(results.sleep_in_atomic.size(), 1U);
  EXPECT_GE(results.mix_raw_and_cooked.size(), 1U);
  EXPECT_EQ(results.irq_save_restore_issues.size(), 1U);
}

TEST_F(LinuxKernelAnalysisTest, FindsRCUWaitCompletionAndTimerIssues) {
  const char *source = R"(
    @rcu_head = global i8* null, align 8
    @wait_q = global i8 0, align 1
    @other_q = global i8 0, align 1
    @comp_done = global i8 0, align 1
    @comp_missing = global i8 0, align 1
    @timer_leak = global i8 0, align 1
    @timer_uad = global i8 0, align 1

    declare void @rcu_read_lock()
    declare void @rcu_read_unlock()
    declare i8* @rcu_dereference(i8**)
    declare void @rcu_assign_pointer(i8**, i8*)
    declare void @synchronize_rcu()
    declare void @call_rcu(i8**, i8*)
    declare void @wait_event(i8*, i32)
    declare void @wake_up(i8*)
    declare void @init_completion(i8*)
    declare void @wait_for_completion(i8*)
    declare void @complete(i8*)
    declare void @timer_setup(i8*, i8*, i32)
    declare void @mod_timer(i8*, i64)
    declare i32 @del_timer_sync(i8*)

    define void @rcu_reader_and_writer() {
    entry:
      call void @rcu_read_lock()
      %inside = call i8* @rcu_dereference(i8** @rcu_head)
      call void @rcu_assign_pointer(i8** @rcu_head, i8* %inside)
      call void @rcu_read_unlock()
      ret void
    }

    define void @rcu_outside_section() {
    entry:
      %before = call i8* @rcu_dereference(i8** @rcu_head)
      call void @synchronize_rcu()
      %after = call i8* @rcu_dereference(i8** @rcu_head)
      call void @call_rcu(i8** @rcu_head, i8* null)
      call void @call_rcu(i8** @rcu_head, i8* null)
      ret void
    }

    define void @waits_and_timers() {
    entry:
      call void @wait_event(i8* @wait_q, i32 1)
      call void @init_completion(i8* @comp_done)
      call void @wait_for_completion(i8* @comp_done)
      call void @init_completion(i8* @comp_missing)
      call void @wait_for_completion(i8* @comp_missing)
      call void @timer_setup(i8* @timer_leak, i8* null, i32 0)
      call void @mod_timer(i8* @timer_leak, i64 100)
      call void @timer_setup(i8* @timer_uad, i8* null, i32 0)
      %deleted = call i32 @del_timer_sync(i8* @timer_uad)
      call void @mod_timer(i8* @timer_uad, i64 200)
      ret void
    }

    define void @wake_and_complete() {
    entry:
      call void @wake_up(i8* @other_q)
      call void @complete(i8* @comp_done)
      call void @complete(i8* @comp_done)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &results = analysis.getResults();

  EXPECT_EQ(results.rcu_without_grace_period.size(), 0U);
  EXPECT_EQ(results.rcu_conflicts.size(), 0U);
  EXPECT_EQ(results.rcu_double_free.size(), 0U);
  EXPECT_EQ(results.deref_after_free.size(), 0U);
  EXPECT_EQ(analysis.getRCUAnalysis().findDerefInWrongSection().size(), 2U);

  EXPECT_EQ(results.missing_wake_ups.size(), 0U);
  EXPECT_EQ(results.spurious_wake_ups.size(), 0U);
  EXPECT_EQ(results.missing_completion.size(), 0U);
  EXPECT_EQ(results.double_completion.size(), 0U);
  EXPECT_EQ(results.timer_not_deleted.size(), 0U);
  EXPECT_EQ(results.timer_use_after_delete.size(), 0U);
  EXPECT_EQ(results.timer_issues.size(), 0U);
}

TEST_F(LinuxKernelAnalysisTest, PreservesStructFieldLockIdentity) {
  const char *source = R"(
    %dev = type { i8, i8 }
    @device = global %dev zeroinitializer

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)

    define void @rx_then_tx() {
    entry:
      call void @spin_lock(i8* getelementptr (%dev, %dev* @device, i32 0, i32 0))
      call void @spin_lock(i8* getelementptr (%dev, %dev* @device, i32 0, i32 1))
      call void @spin_unlock(i8* getelementptr (%dev, %dev* @device, i32 0, i32 1))
      call void @spin_unlock(i8* getelementptr (%dev, %dev* @device, i32 0, i32 0))
      ret void
    }

    define void @tx_then_rx() {
    entry:
      call void @spin_lock(i8* getelementptr (%dev, %dev* @device, i32 0, i32 1))
      call void @spin_lock(i8* getelementptr (%dev, %dev* @device, i32 0, i32 0))
      call void @spin_unlock(i8* getelementptr (%dev, %dev* @device, i32 0, i32 0))
      call void @spin_unlock(i8* getelementptr (%dev, %dev* @device, i32 0, i32 1))
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  std::set<LockID> locks;
  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.kind == OperationKind::LOCK_ACQUIRE) {
      locks.insert(op.lock);
    }
  }
  EXPECT_EQ(locks.size(), 2U);
  EXPECT_TRUE(analysis.getResults().double_locks.empty());
  EXPECT_EQ(analysis.getResults().lock_order_inversions.size(), 1U);
}

TEST_F(LinuxKernelAnalysisTest, SummarizesThinLockWrappersAtCallSites) {
  const char *source = R"(
    @a = global i8 0
    @b = global i8 0

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)

    define void @my_spin_lock(i32 %tag, i8* %lock) {
    entry:
      call void @spin_lock(i8* %lock)
      ret void
    }

    define void @my_spin_unlock(i32 %tag, i8* %lock) {
    entry:
      call void @spin_unlock(i8* %lock)
      ret void
    }

    define void @one() {
    entry:
      call void @my_spin_lock(i32 1, i8* @a)
      call void @my_spin_lock(i32 2, i8* @b)
      call void @my_spin_unlock(i32 2, i8* @b)
      call void @my_spin_unlock(i32 1, i8* @a)
      ret void
    }

    define void @two() {
    entry:
      call void @my_spin_lock(i32 2, i8* @b)
      call void @my_spin_lock(i32 1, i8* @a)
      call void @my_spin_unlock(i32 1, i8* @a)
      call void @my_spin_unlock(i32 2, i8* @b)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(OperationKind::LOCK_ACQUIRE), 4U);
  EXPECT_EQ(analysis.getResults().lock_order_inversions.size(), 1U);
  EXPECT_TRUE(analysis.getResults().lock_without_unlock.empty());
}

TEST_F(LinuxKernelAnalysisTest, ResolvesConstantFunctionPointerLockCalls) {
  const char *source = R"(
    @a = global i8 0
    @b = global i8 0
    @lock_fn = constant void (i8*)* @spin_lock
    @unlock_fn = constant void (i8*)* @spin_unlock

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)

    define void @indirect_one() {
    entry:
      %lock = load void (i8*)*, void (i8*)** @lock_fn
      %unlock = load void (i8*)*, void (i8*)** @unlock_fn
      call void %lock(i8* @a)
      call void %lock(i8* @b)
      call void %unlock(i8* @b)
      call void %unlock(i8* @a)
      ret void
    }

    define void @indirect_two() {
    entry:
      %lock = load void (i8*)*, void (i8*)** @lock_fn
      %unlock = load void (i8*)*, void (i8*)** @unlock_fn
      call void %lock(i8* @b)
      call void %lock(i8* @a)
      call void %unlock(i8* @a)
      call void %unlock(i8* @b)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().lock_order_inversions.size(), 1U);
  EXPECT_TRUE(analysis.getResults().lock_without_unlock.empty());
}

TEST_F(LinuxKernelAnalysisTest, RecordsAsynchronousKernelCallbacks) {
  const char *source = R"(
    @work = global i8 0
    @wq = global i8 0
    @timer = global i8 0
    @head = global i8 0
    @dev = global i8 0

    define void @worker() { ret void }
    define void @timer_fn() { ret void }
    define void @irq_fn() { ret void }
    define void @threaded_irq_fn() { ret void }
    define void @rcu_fn() { ret void }
    define void @thread_fn() { ret void }

    declare i8* @kthread_create(void ()*, i8*)
    declare void @__init_work(i8*, void ()*)
    declare i1 @queue_work(i8*, i8*)
    declare void @timer_setup(i8*, void ()*, i32)
    declare i32 @request_threaded_irq(i32, void ()*, void ()*, i64, i8*, i8*)
    declare void @call_rcu(i8*, void ()*)

    define void @register_callbacks() {
    entry:
      %task = call i8* @kthread_create(void ()* @thread_fn, i8* null)
      call void @__init_work(i8* @work, void ()* @worker)
      %queued = call i1 @queue_work(i8* @wq, i8* @work)
      call void @timer_setup(i8* @timer, void ()* @timer_fn, i32 0)
      %irq = call i32 @request_threaded_irq(i32 7, void ()* @irq_fn,
                                            void ()* @threaded_irq_fn,
                                            i64 0, i8* null, i8* @dev)
      call void @call_rcu(i8* @head, void ()* @rcu_fn)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_work_submit = false;
  bool saw_threaded_irq = false;
  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.kind == OperationKind::WORKqueue_SUBMIT) {
      saw_work_submit = true;
      EXPECT_EQ(op.callback, module->getFunction("worker"));
      EXPECT_EQ(op.async_context, AsyncContextKind::WORKQUEUE);
    }
    if (op.kind == OperationKind::IRQ_REQUEST) {
      saw_threaded_irq = true;
      EXPECT_EQ(op.callbacks.size(), 2U);
    }
    if (op.kind == OperationKind::TIMER_SETUP ||
        op.kind == OperationKind::RCU_CALL ||
        op.kind == OperationKind::KTHREAD_CREATE) {
      EXPECT_NE(op.callback, nullptr);
      EXPECT_NE(op.async_context, AsyncContextKind::NONE);
    }
  }
  EXPECT_TRUE(saw_work_submit);
  EXPECT_TRUE(saw_threaded_irq);
}

TEST_F(LinuxKernelAnalysisTest, UsesCFGStateForBranchesLoopsAndFunctionExits) {
  const char *source = R"(
    @a = global i8 0
    @b = global i8 0
    @loop_lock = global i8 0
    @cross = global i8 0

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)

    define void @diamond_one(i1 %choose) {
    entry:
      br i1 %choose, label %left, label %right
    left:
      call void @spin_lock(i8* @a)
      br label %exit
    right:
      call void @spin_lock(i8* @b)
      br label %exit
    exit:
      ret void
    }

    define void @diamond_two(i1 %choose) {
    entry:
      br i1 %choose, label %left, label %right
    left:
      call void @spin_lock(i8* @b)
      br label %exit
    right:
      call void @spin_lock(i8* @a)
      br label %exit
    exit:
      ret void
    }

    define void @balanced_loop(i1 %again) {
    entry:
      br label %loop
    loop:
      call void @spin_lock(i8* @loop_lock)
      call void @spin_unlock(i8* @loop_lock)
      br i1 %again, label %loop, label %exit
    exit:
      ret void
    }

    define void @cross_acquire() {
    entry:
      call void @spin_lock(i8* @cross)
      ret void
    }

    define void @cross_release() {
    entry:
      call void @spin_unlock(i8* @cross)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &results = analysis.getResults();

  EXPECT_TRUE(results.lock_order_inversions.empty());
  EXPECT_EQ(results.lock_without_unlock.size(), 5U);
  EXPECT_EQ(results.unlock_without_lock.size(), 1U);
  EXPECT_TRUE(
      llvm::none_of(results.lock_without_unlock, [](const Instruction *inst) {
        return inst->getFunction()->getName() == "balanced_loop";
      }));
}

TEST_F(LinuxKernelAnalysisTest, RefinesConditionalLocksAndHonorsRWLockModes) {
  const char *source = R"(
    @a = global i8 0
    @b = global i8 0
    @ra = global i8 0
    @rb = global i8 0
    @rc = global i8 0
    @rd = global i8 0
    @sem = global i8 0
    @c = global i8 0

    declare i32 @mutex_trylock(i8*)
    declare void @mutex_unlock(i8*)
    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)
    declare void @read_lock(i8*)
    declare void @read_unlock(i8*)
    declare void @write_lock(i8*)
    declare void @write_unlock(i8*)
    declare i32 @down_trylock(i8*)
    declare void @up(i8*)

    define void @try_path() {
    entry:
      %ok = call i32 @mutex_trylock(i8* @a)
      %failed = icmp eq i32 %ok, 0
      br i1 %failed, label %failure, label %success
    success:
      call void @spin_lock(i8* @b)
      call void @spin_unlock(i8* @b)
      call void @mutex_unlock(i8* @a)
      ret void
    failure:
      ret void
    }

    define void @reverse_try_order() {
    entry:
      call void @spin_lock(i8* @b)
      %ignored = call i32 @mutex_trylock(i8* @a)
      call void @spin_unlock(i8* @b)
      ret void
    }

    define void @read_read_one() {
    entry:
      call void @read_lock(i8* @ra)
      call void @read_lock(i8* @rb)
      call void @read_unlock(i8* @rb)
      call void @read_unlock(i8* @ra)
      ret void
    }

    define void @read_read_two() {
    entry:
      call void @read_lock(i8* @rb)
      call void @read_lock(i8* @ra)
      call void @read_unlock(i8* @ra)
      call void @read_unlock(i8* @rb)
      ret void
    }

    define void @read_write_one() {
    entry:
      call void @read_lock(i8* @rc)
      call void @write_lock(i8* @rd)
      call void @write_unlock(i8* @rd)
      call void @read_unlock(i8* @rc)
      ret void
    }

    define void @read_write_two() {
    entry:
      call void @read_lock(i8* @rd)
      call void @write_lock(i8* @rc)
      call void @write_unlock(i8* @rc)
      call void @read_unlock(i8* @rd)
      ret void
    }

    define void @semaphore_try_path() {
    entry:
      %ret = call i32 @down_trylock(i8* @sem)
      %failed = icmp ne i32 %ret, 0
      br i1 %failed, label %failure, label %success
    success:
      call void @spin_lock(i8* @c)
      call void @spin_unlock(i8* @c)
      call void @up(i8* @sem)
      ret void
    failure:
      ret void
    }

    define void @reverse_semaphore_order() {
    entry:
      call void @spin_lock(i8* @c)
      %ignored = call i32 @down_trylock(i8* @sem)
      call void @spin_unlock(i8* @c)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().lock_order_inversions.size(), 3U);
  EXPECT_TRUE(analysis.getResults().lock_deadlocks.empty());
}
