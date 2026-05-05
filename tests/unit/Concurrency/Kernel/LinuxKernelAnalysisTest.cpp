#include "Concurrency/LinuxKernel/LinuxKernelAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

#include <algorithm>

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
  EXPECT_EQ(results.lock_deadlocks.size(), 1U);
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
    declare void @synchronize_rcu(i8**)
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
      call void @synchronize_rcu(i8** @rcu_head)
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

  EXPECT_EQ(results.rcu_without_grace_period.size(), 1U);
  EXPECT_EQ(results.rcu_conflicts.size(), 1U);
  EXPECT_EQ(results.rcu_double_free.size(), 1U);
  EXPECT_EQ(results.deref_after_free.size(), 1U);
  EXPECT_EQ(analysis.getRCUAnalysis().findDerefInWrongSection().size(), 2U);

  EXPECT_EQ(results.missing_wake_ups.size(), 1U);
  EXPECT_EQ(results.spurious_wake_ups.size(), 1U);
  EXPECT_EQ(results.missing_completion.size(), 1U);
  EXPECT_EQ(results.double_completion.size(), 1U);
  EXPECT_EQ(results.timer_not_deleted.size(), 1U);
  EXPECT_EQ(results.timer_use_after_delete.size(), 1U);
  EXPECT_EQ(results.timer_issues.size(), 1U);
}
