#include "LinuxKernelAnalysisTestSupport.h"

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

TEST_F(LinuxKernelAnalysisTest,
       PromotesInversionsWithExplicitKernelConcurrencyEvidence) {
  const char *source = R"(
    @a = global i8 0
    @b = global i8 0

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)
    declare i8* @kthread_run(void ()*, i8*)

    define void @thread_ab() {
    entry:
      call void @spin_lock(i8* @a)
      call void @spin_lock(i8* @b)
      call void @spin_unlock(i8* @b)
      call void @spin_unlock(i8* @a)
      ret void
    }

    define void @thread_ba() {
    entry:
      call void @spin_lock(i8* @b)
      call void @spin_lock(i8* @a)
      call void @spin_unlock(i8* @a)
      call void @spin_unlock(i8* @b)
      ret void
    }

    define void @start_threads() {
    entry:
      %first = call i8* @kthread_run(void ()* @thread_ab, i8* null)
      %second = call i8* @kthread_run(void ()* @thread_ba, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getExecutionGraph().getContexts().size(), 2U);
  EXPECT_EQ(analysis.getExecutionGraph()
                .getContextsForFunction(module->getFunction("thread_ab"))
                .size(),
            1U);
  EXPECT_EQ(analysis.getResults().lock_order_inversions.size(), 1U);
  EXPECT_EQ(analysis.getResults().lock_deadlocks.size(), 1U);
}

TEST_F(LinuxKernelAnalysisTest,
       RegistrationWithoutSubmissionDoesNotInventConcurrency) {
  const char *source = R"(
    @a = global i8 0
    @b = global i8 0

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)
    declare i8* @kthread_create(void ()*, i8*)

    define void @thread_ab() {
    entry:
      call void @spin_lock(i8* @a)
      call void @spin_lock(i8* @b)
      call void @spin_unlock(i8* @b)
      call void @spin_unlock(i8* @a)
      ret void
    }

    define void @thread_ba() {
    entry:
      call void @spin_lock(i8* @b)
      call void @spin_lock(i8* @a)
      call void @spin_unlock(i8* @a)
      call void @spin_unlock(i8* @b)
      ret void
    }

    define void @create_threads() {
    entry:
      %first = call i8* @kthread_create(void ()* @thread_ab, i8* null)
      %second = call i8* @kthread_create(void ()* @thread_ba, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getExecutionGraph().getContexts().size(), 2U);
  EXPECT_EQ(analysis.getResults().lock_order_inversions.size(), 1U);
  EXPECT_TRUE(analysis.getResults().lock_deadlocks.empty());
}

TEST_F(LinuxKernelAnalysisTest,
       SeparatesLockInstancesFromLockClassesAndSubclasses) {
  const char *source = R"(
    %device = type { i8, i8 }

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)
    declare void @spin_lock_nested(i8*, i32)

    define void @lock_instances(%device* %left, %device* %right) {
    entry:
      %left_lock = getelementptr %device, %device* %left, i32 0, i32 1
      %right_lock = getelementptr %device, %device* %right, i32 0, i32 1
      call void @spin_lock(i8* %left_lock)
      call void @spin_unlock(i8* %left_lock)
      call void @spin_lock(i8* %right_lock)
      call void @spin_unlock(i8* %right_lock)
      call void @spin_lock_nested(i8* %left_lock, i32 3)
      call void @spin_unlock(i8* %left_lock)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  std::vector<const KernelOperation *> acquires;
  for (const KernelOperation &op :
       analysis.getProcessModel().getAllOperations()) {
    if (op.kind == OperationKind::LOCK_ACQUIRE) {
      acquires.push_back(&op);
    }
  }
  ASSERT_EQ(acquires.size(), 3U);
  EXPECT_NE(acquires[0]->lock, acquires[1]->lock);
  EXPECT_EQ(acquires[0]->lock_class, acquires[1]->lock_class);
  EXPECT_NE(acquires[0]->lock_class, acquires[2]->lock_class);
  EXPECT_EQ(acquires[2]->lock_subclass, 3U);
}

TEST_F(LinuxKernelAnalysisTest,
       PropagatesOrderedMultiEffectLockSummariesAcrossWrappers) {
  const char *source = R"(
    @a = global i8 0
    @b = global i8 0

    declare void @spin_lock(i8*)
    declare void @spin_unlock(i8*)

    define void @lock_pair(i8* %first, i8* %second) {
    entry:
      call void @spin_lock(i8* %first)
      call void @spin_lock(i8* %second)
      ret void
    }

    define void @unlock_pair(i8* %first, i8* %second) {
    entry:
      call void @spin_unlock(i8* %second)
      call void @spin_unlock(i8* %first)
      ret void
    }

    define void @nested_lock_pair(i8* %first, i8* %second) {
    entry:
      call void @lock_pair(i8* %first, i8* %second)
      ret void
    }

    define void @path_ab() {
    entry:
      call void @nested_lock_pair(i8* @a, i8* @b)
      call void @unlock_pair(i8* @a, i8* @b)
      ret void
    }

    define void @path_ba() {
    entry:
      call void @nested_lock_pair(i8* @b, i8* @a)
      call void @unlock_pair(i8* @b, i8* @a)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LinuxKernelAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().lock_order_inversions.size(), 1U);
  EXPECT_TRUE(analysis.getResults().lock_without_unlock.empty());
  EXPECT_TRUE(analysis.getResults().unlock_without_lock.empty());
  EXPECT_EQ(analysis.getOperationCount(OperationKind::LOCK_ACQUIRE), 4U);
  EXPECT_EQ(analysis.getOperationCount(OperationKind::LOCK_RELEASE), 4U);
}
