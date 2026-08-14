#include "Concurrency/LinuxKernel/LinuxKernelAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <set>

using namespace llvm;
using namespace kernel;
using namespace lotus::unittest;

class LinuxKernelAnalysisTest : public LlvmModuleTest {};

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

TEST_F(LinuxKernelAnalysisTest, ModelsWaitCompletionAndTimerContracts) {
  const char *source = R"(
    @q_true = global i8 0
    @q_false = global i8 0
    @done = global i8 0
    @all_done = global i8 0
    @timer = global i8 0
    @shutdown_timer = global i8 0
    @branch_done = global i8 0
    @branch_timer = global i8 0

    declare void @wait_event(i8*, i1)
    declare void @complete(i8*)
    declare void @complete_all(i8*)
    declare void @init_completion(i8*)
    declare void @reinit_completion(i8*)
    declare i32 @timer_delete_sync(i8*)
    declare i32 @timer_shutdown_sync(i8*)
    declare void @mod_timer(i8*, i64)

    define void @contracts() {
    entry:
      call void @wait_event(i8* @q_true, i1 true)
      call void @wait_event(i8* @q_false, i1 false)
      call void @init_completion(i8* @done)
      call void @complete(i8* @done)
      call void @complete(i8* @done)
      call void @init_completion(i8* @all_done)
      call void @complete_all(i8* @all_done)
      call void @complete_all(i8* @all_done)
      call void @reinit_completion(i8* @all_done)
      call void @complete_all(i8* @all_done)
      %cancelled = call i32 @timer_delete_sync(i8* @timer)
      call void @mod_timer(i8* @timer, i64 100)
      %shutdown = call i32 @timer_shutdown_sync(i8* @shutdown_timer)
      call void @mod_timer(i8* @shutdown_timer, i64 200)
      ret void
    }

    define void @exclusive_complete_all(i1 %choose) {
    entry:
      call void @init_completion(i8* @branch_done)
      br i1 %choose, label %left, label %right
    left:
      call void @complete_all(i8* @branch_done)
      ret void
    right:
      call void @complete_all(i8* @branch_done)
      ret void
    }

    define void @exclusive_timer_lifecycle(i1 %choose) {
    entry:
      br i1 %choose, label %shutdown, label %rearm
    shutdown:
      %stopped = call i32 @timer_shutdown_sync(i8* @branch_timer)
      ret void
    rearm:
      call void @mod_timer(i8* @branch_timer, i64 300)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &results = analysis.getResults();

  EXPECT_TRUE(results.missing_wake_ups.empty());
  EXPECT_EQ(results.double_completion.size(), 1U);
  EXPECT_TRUE(results.timer_not_deleted.empty());
  EXPECT_EQ(results.timer_use_after_delete.size(), 1U);
  EXPECT_EQ(results.timer_issues.size(), 1U);
}

TEST_F(LinuxKernelAnalysisTest,
       TracksRCUFlavorsNestingAndProtectedDereferences) {
  const char *source = R"(
    @gp = global i8* null
    @srcu_a = global i8 0
    @srcu_b = global i8 0

    declare void @rcu_read_lock()
    declare void @rcu_read_unlock()
    declare i8* @rcu_dereference(i8**)
    declare i8* @rcu_dereference_protected(i8**, i1)
    declare i32 @srcu_read_lock(i8*)
    declare void @srcu_read_unlock(i8*, i32)
    declare void @synchronize_srcu(i8*)
    declare i8* @rcu_replace_pointer(i8**, i8*, i1)
    declare void @synchronize_rcu()
    declare void @kfree(i8*)

    define void @nested_reader() {
    entry:
      call void @rcu_read_lock()
      call void @rcu_read_lock()
      %p = call i8* @rcu_dereference(i8** @gp)
      call void @rcu_read_unlock()
      call void @rcu_read_unlock()
      ret void
    }

    define void @protected_reader() {
    entry:
      %p = call i8* @rcu_dereference_protected(i8** @gp, i1 true)
      ret void
    }

    define void @srcu_domains() {
    entry:
      %idx = call i32 @srcu_read_lock(i8* @srcu_a)
      call void @srcu_read_unlock(i8* @srcu_a, i32 %idx)
      call void @synchronize_srcu(i8* @srcu_b)
      ret void
    }

    define void @safe_reclaim() {
    entry:
      %old = call i8* @rcu_replace_pointer(i8** @gp, i8* null, i1 true)
      call void @synchronize_rcu()
      call void @kfree(i8* %old)
      ret void
    }

    define void @unsafe_reclaim() {
    entry:
      %old = call i8* @rcu_replace_pointer(i8** @gp, i8* null, i1 true)
      call void @kfree(i8* %old)
      call void @synchronize_rcu()
      ret void
    }

    define void @conditionally_synchronized_reclaim(i1 %choose) {
    entry:
      %old = call i8* @rcu_replace_pointer(i8** @gp, i8* null, i1 true)
      br i1 %choose, label %sync, label %skip
    sync:
      call void @synchronize_rcu()
      br label %reclaim
    skip:
      br label %reclaim
    reclaim:
      call void @kfree(i8* %old)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getRCUSectionCount(), 2U);
  EXPECT_TRUE(analysis.getResults().rcu_without_grace_period.empty());
  EXPECT_EQ(analysis.getResults().rcu_unsafe_reclamation.size(), 2U);
  EXPECT_TRUE(analysis.getRCUAnalysis().findDerefInWrongSection().empty());
  const auto sections = analysis.getRCUAnalysis().getReadSideSections();
  auto srcu = llvm::find_if(sections, [](const auto &section) {
    return section.flavor == RCUFlavor::SRCU;
  });
  ASSERT_NE(srcu, sections.end());
  EXPECT_FALSE(srcu->has_sync);
}

TEST_F(LinuxKernelAnalysisTest, ExtractsAtomicOperandsByAPI) {
  const char *source = R"(
    @counter = global i32 0
    @flags = global i64 0

    declare void @atomic_set(i32*, i32)
    declare i32 @atomic_add_return(i32, i32*)
    declare i32 @atomic_cmpxchg(i32*, i32, i32)
    declare void @atomic_inc(i32*)
    declare void @set_bit(i32, i64*)
    declare i1 @test_bit(i32, i64*)

    define void @atomics() {
    entry:
      call void @atomic_set(i32* @counter, i32 4)
      %add = call i32 @atomic_add_return(i32 1, i32* @counter)
      %cmp = call i32 @atomic_cmpxchg(i32* @counter, i32 5, i32 6)
      call void @atomic_inc(i32* @counter)
      call void @set_bit(i32 3, i64* @flags)
      %test = call i1 @test_bit(i32 3, i64* @flags)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.kind != OperationKind::ATOMIC_READ &&
        op.kind != OperationKind::ATOMIC_WRITE &&
        op.kind != OperationKind::ATOMIC_RMW) {
      continue;
    }
    if (op.function_name == "set_bit" || op.function_name == "test_bit") {
      EXPECT_EQ(op.atomic_var, module->getNamedGlobal("flags"));
    } else {
      EXPECT_EQ(op.atomic_var, module->getNamedGlobal("counter"));
    }
  }
}

TEST_F(LinuxKernelAnalysisTest,
       DistinguishesLocalAndPerLineIRQStateAndIsIdempotent) {
  const char *source = R"(
    @mutex = global i8 0
    declare void @disable_irq(i32)
    declare void @local_irq_disable()
    declare void @local_irq_enable()
    declare void @local_irq_save(i64*)
    declare void @local_irq_restore(i64*)
    declare void @mutex_lock(i8*)
    declare void @mutex_unlock(i8*)

    define void @line_irq() {
    entry:
      call void @disable_irq(i32 7)
      call void @mutex_lock(i8* @mutex)
      call void @mutex_unlock(i8* @mutex)
      ret void
    }

    define void @local_irq() {
    entry:
      call void @local_irq_disable()
      call void @mutex_lock(i8* @mutex)
      call void @mutex_unlock(i8* @mutex)
      call void @local_irq_enable()
      ret void
    }

    define void @exclusive_irq_branch(i1 %choose) {
    entry:
      br i1 %choose, label %disabled, label %sleeping
    disabled:
      call void @local_irq_disable()
      ret void
    sleeping:
      call void @mutex_lock(i8* @mutex)
      call void @mutex_unlock(i8* @mutex)
      ret void
    }

    define void @swapped_irq_flags() {
    entry:
      %first = alloca i64
      %second = alloca i64
      call void @local_irq_save(i64* %first)
      call void @local_irq_restore(i64* %second)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().sleep_in_atomic.size(), 1U);
  EXPECT_EQ(
      analysis.getResults().sleep_in_atomic.front()->getFunction()->getName(),
      "local_irq");
  EXPECT_EQ(analysis.getResults().irq_save_restore_issues.size(), 2U);

  const auto diagnostics = analysis.getLockAnalysis().getLockDiagnostics();
  EXPECT_EQ(analysis.getLockAnalysis().findLockOrderInversions(),
            analysis.getLockAnalysis().findLockOrderInversions());
  EXPECT_EQ(diagnostics, analysis.getLockAnalysis().getLockDiagnostics());

  const auto first_results = analysis.getResults().sleep_in_atomic;
  analysis.runAnalysis();
  EXPECT_EQ(first_results, analysis.getResults().sleep_in_atomic);
}

TEST_F(LinuxKernelAnalysisTest, MakesPreemptRTAssumptionExplicit) {
  const char *source = R"(
    @spin = global i8 0
    @mutex = global i8 0
    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)
    declare void @mutex_lock(i8*)
    declare void @mutex_unlock(i8*)

    define void @critical_section() {
    entry:
      call void @spin_lock(i8* @spin)
      call void @mutex_lock(i8* @mutex)
      call void @mutex_unlock(i8* @mutex)
      call void @spin_unlock(i8* @spin)
      ret void
    }
  )";

  auto non_rt_module = parseModule(source);
  ASSERT_NE(non_rt_module, nullptr);
  LinuxKernelAnalysis non_rt(*non_rt_module, false);
  non_rt.runAnalysis();
  EXPECT_EQ(non_rt.getResults().sleep_in_atomic.size(), 1U);

  auto rt_module = parseModule(source);
  ASSERT_NE(rt_module, nullptr);
  LinuxKernelAnalysis rt(*rt_module, true);
  rt.runAnalysis();
  EXPECT_TRUE(rt.getResults().sleep_in_atomic.empty());
}
