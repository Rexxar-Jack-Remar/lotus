#include "LockSetAnalysisTestSupport.h"

#include <llvm/IR/IRBuilder.h>

TEST_F(LockSetAnalysisTest, SharedLockSummaryPreservesReadModeAcrossCalls) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      call void @helper()
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_TRUE(lsa.mustHoldLock(after, lock));
  EXPECT_TRUE(lsa.getMayReadLockSetAt(after).count(lock) > 0);
  EXPECT_TRUE(lsa.getMayWriteLockSetAt(after).count(lock) == 0);
  EXPECT_TRUE(lsa.getMustReadLockSetAt(after).count(lock) > 0);
  EXPECT_TRUE(lsa.getMustWriteLockSetAt(after).count(lock) == 0);
}

TEST_F(LockSetAnalysisTest,
       CalleeHeldExitLocksBecomeCallerMustLocksWhenDefinitelyAcquired) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      call void @helper()
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_TRUE(lsa.mustHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest,
       HelperUnlockDropsCallerMustLockStateButPreservesMayWhenConditional) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0
    @flag = external global i1

    define void @maybe_unlock() {
    entry:
      %cond = load i1, i1* @flag
      br i1 %cond, label %unlock, label %done

    unlock:
      call i32 @pthread_mutex_unlock(i8* @lock)
      br label %done

    done:
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      call void @maybe_unlock()
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest,
       MixedBalancedAndBareUnlockStillDropsCallerMustLockState) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @helper(i1 %take_internal) {
    entry:
      br i1 %take_internal, label %take, label %release

    take:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %release

    release:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }

    define i32 @main(i1 %cond) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      call void @helper(i1 %cond)
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest,
       UnresolvedIndirectCallClearsMustLockStateConservatively) {
  const char *source = R"(
    @hook = external global void ()*

    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @unlock_helper() {
    entry:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %fn = load void ()*, void ()** @hook
      call void %fn()
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest, BalancedRaiiHelperDoesNotClearCallerMustLockState) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @fake_unique_lock_D1Ev(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      %ul = alloca i8
      call void @fake_unique_lock_C1E(i8* %ul, i8* @lock)
      call void @fake_unique_lock_D1Ev(i8* %ul)
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      call void @helper()
      %after = add i32 1, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mustHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest,
       LeadingNonCallBeforeHelperAcquireStillAppliesSummary) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define void @lock_helper() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      %seed = add i32 0, 1
      call void @lock_helper()
      %after = add i32 %seed, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest,
       LeadingNonCallBeforeHelperReleaseClearsCallerMustState) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define void @unlock_helper() {
    entry:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %seed = add i32 0, 1
      call void @unlock_helper()
      %after = add i32 %seed, 2
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(lock, nullptr);

  EXPECT_FALSE(lsa.mayHoldLock(after, lock));
  EXPECT_FALSE(lsa.mustHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest, SummaryInstantiatesFormalToActual) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @actual = global i8 0

    define void @acquire(i8* %m) {
    entry:
      call i32 @pthread_mutex_lock(i8* %m)
      ret void
    }

    define void @release(i8* %m) {
    entry:
      call i32 @pthread_mutex_unlock(i8* %m)
      ret void
    }

    define void @main() {
    entry:
      call void @acquire(i8* @actual)
      %held = add i32 1, 2
      call void @release(i8* @actual)
      %released = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *main = module->getFunction("main");
  const Instruction *held = findInstructionByName(*main, "held");
  const Instruction *released = findInstructionByName(*main, "released");
  const GlobalVariable *actual = module->getNamedGlobal("actual");
  ASSERT_NE(held, nullptr);
  ASSERT_NE(released, nullptr);
  EXPECT_EQ(lsa.getMustLockSetAt(held).count(actual), 1u);
  EXPECT_EQ(lsa.getMayLockSetAt(released).count(actual), 0u);
}

TEST_F(LockSetAnalysisTest, SummaryInstantiatesConstantFormalProjection) {
  const char *source = R"(
    %pair = type { i8, i8 }

    declare i32 @pthread_mutex_lock(i8*)

    @actual = global %pair zeroinitializer

    define void @acquire_second(%pair* %base) {
    entry:
      %field = getelementptr %pair, %pair* %base, i32 0, i32 1
      call i32 @pthread_mutex_lock(i8* %field)
      ret void
    }

    define void @main() {
    entry:
      call void @acquire_second(%pair* @actual)
      %after = add i32 1, 2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  Constant *zero = ConstantInt::get(Type::getInt32Ty(module->getContext()), 0);
  Constant *one = ConstantInt::get(Type::getInt32Ty(module->getContext()), 1);
  Constant *indices[] = {zero, one};
  Constant *field = ConstantExpr::getGetElementPtr(
      StructType::getTypeByName(module->getContext(), "pair"),
      module->getNamedGlobal("actual"), indices);
  ASSERT_NE(after, nullptr);
  EXPECT_TRUE(lsa.mustHoldLock(after, field));
}

TEST_F(LockSetAnalysisTest, ExceptionalSummaryFlowsOnlyToInvokeUnwindEdge) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)
    declare void @may_throw()
    declare i32 @__gxx_personality_v0(...)

    @lock = global i8 0

    define void @callee() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      invoke void @may_throw() to label %normal unwind label %lpad

    normal:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }

    define void @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      invoke void @callee() to label %cont unwind label %catch

    cont:
      %normal_marker = add i32 1, 2
      ret void

    catch:
      %caught = landingpad { i8*, i32 } cleanup
      %unwind_marker = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *caller = module->getFunction("caller");
  const Instruction *normal = findInstructionByName(*caller, "normal_marker");
  const Instruction *unwind = findInstructionByName(*caller, "unwind_marker");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(normal, nullptr);
  ASSERT_NE(unwind, nullptr);
  ASSERT_NE(lock, nullptr);
  EXPECT_FALSE(lsa.mayHoldLock(normal, lock));
  EXPECT_TRUE(lsa.mayHoldLock(unwind, lock));
}

TEST_F(LockSetAnalysisTest, RecursiveCallGraphReachesSummaryFixpoint) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define void @a(i1 %base) {
    entry:
      br i1 %base, label %done, label %recurse
    recurse:
      call void @b(i1 true)
      br label %done
    done:
      ret void
    }

    define void @b(i1 %base) {
    entry:
      br i1 %base, label %lock_path, label %recurse
    lock_path:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %done
    recurse:
      call void @a(i1 true)
      br label %done
    done:
      ret void
    }

    define void @main() {
    entry:
      call void @a(i1 false)
      %after = add i32 1, 2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  EXPECT_TRUE(lsa.mayHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest, CalleeOnlyLockSurvivesMustLoopMeet) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      ret void
    }

    define void @main(i1 %again) {
    entry:
      call void @helper()
      br label %loop
    loop:
      %inside = add i32 1, 2
      br i1 %again, label %loop, label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Instruction *inside =
      findInstructionByName(*module->getFunction("main"), "inside");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(inside, nullptr);
  EXPECT_TRUE(lsa.mustHoldLock(inside, lock));
}

TEST_F(LockSetAnalysisTest, ReanalyzeRebuildsOwnedCallGraph) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define void @helper() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      ret void
    }

    define void @main() {
    entry:
      %after = add i32 1, 2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();

  Function *main = module->getFunction("main");
  Function *helper = module->getFunction("helper");
  Instruction *after = findInstructionByName(*main, "after");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  ASSERT_NE(after, nullptr);
  EXPECT_FALSE(lsa.mayHoldLock(after, lock));

  IRBuilder<> builder(after);
  builder.CreateCall(helper);
  lsa.analyze();

  LockSetAnalysis fresh(*module);
  fresh.analyze();
  EXPECT_EQ(lsa.getMayLockSetAt(after), fresh.getMayLockSetAt(after));
  EXPECT_EQ(lsa.getMustLockSetAt(after), fresh.getMustLockSetAt(after));
  EXPECT_TRUE(lsa.mustHoldLock(after, lock));
}

TEST_F(LockSetAnalysisTest, SummaryComposesInstructionGEPProjection) {
  const char *source = R"(
    %pair = type { i8, i8 }
    %outer = type { i8, %pair }
    declare i32 @pthread_mutex_lock(i8*)
    @actual = global %outer zeroinitializer
    define void @acquire_second(%pair* %base) {
    entry:
      %field = getelementptr %pair, %pair* %base, i32 0, i32 1
      call i32 @pthread_mutex_lock(i8* %field)
      ret void
    }
    define void @main() {
    entry:
      %inner = getelementptr %outer, %outer* @actual, i32 0, i32 1
      call void @acquire_second(%pair* %inner)
      %after = add i32 1, 2
      ret void
    }
    define void @forward(%pair* %base) {
    entry:
      call void @acquire_second(%pair* %base)
      %after_forward = add i32 3, 4
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  Function *main = module->getFunction("main");
  const Instruction *after = findInstructionByName(*main, "after");
  const Instruction *inner = findInstructionByName(*main, "inner");
  EXPECT_FALSE(lsa.mustHoldLock(after, inner));
  const LockSet held = lsa.getMustLockSetAt(after);
  ASSERT_EQ(held.size(), 1u);
  const auto *projected = dyn_cast<GetElementPtrInst>(*held.begin());
  ASSERT_NE(projected, nullptr);
  EXPECT_EQ(projected->getPointerOperand(), inner);
  EXPECT_TRUE(projected->hasAllConstantIndices());
  Function *forward = module->getFunction("forward");
  const Instruction *after_forward =
      findInstructionByName(*forward, "after_forward");
  const LockSet forward_held = lsa.getMustLockSetAt(after_forward);
  ASSERT_EQ(forward_held.size(), 1u);
  const auto *forward_projected =
      dyn_cast<GetElementPtrInst>(*forward_held.begin());
  ASSERT_NE(forward_projected, nullptr);
  EXPECT_EQ(forward_projected->getPointerOperand(), forward->getArg(0));
}

TEST_F(LockSetAnalysisTest, RecursiveDepthComposesAcrossSummary) {
  const char *source = R"(
    declare void @_ZNSt15recursive_mutex4lockEv(i8*)
    declare void @_ZNSt15recursive_mutex6unlockEv(i8*)
    @recursive = global i8 0
    define void @helper(i8* %m) {
    entry:
      call void @_ZNSt15recursive_mutex4lockEv(i8* %m)
      ret void
    }
    define void @main() {
    entry:
      call void @_ZNSt15recursive_mutex4lockEv(i8* @recursive)
      call void @helper(i8* @recursive)
      call void @_ZNSt15recursive_mutex6unlockEv(i8* @recursive)
      %after = add i32 1, 2
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *recursive = module->getNamedGlobal("recursive");
  EXPECT_TRUE(lsa.mayHoldLock(after, recursive));
  EXPECT_TRUE(lsa.mustHoldLock(after, recursive));
}

TEST_F(LockSetAnalysisTest, RecursiveReleaseDepthComposesAcrossSummary) {
  const char *source = R"(
    declare void @_ZNSt15recursive_mutex4lockEv(i8*)
    declare void @_ZNSt15recursive_mutex6unlockEv(i8*)
    @recursive = global i8 0
    define void @helper(i8* %m) {
    entry:
      call void @_ZNSt15recursive_mutex6unlockEv(i8* %m)
      ret void
    }
    define void @main() {
    entry:
      call void @_ZNSt15recursive_mutex4lockEv(i8* @recursive)
      call void @_ZNSt15recursive_mutex4lockEv(i8* @recursive)
      call void @helper(i8* @recursive)
      %after = add i32 1, 2
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *recursive = module->getNamedGlobal("recursive");
  EXPECT_TRUE(lsa.mayHoldLock(after, recursive));
  EXPECT_TRUE(lsa.mustHoldLock(after, recursive));
  EXPECT_EQ(lsa.getLockNestingDepth(after), 1u);
}

TEST_F(LockSetAnalysisTest, RecursiveDepthComposesAcrossSummarySCC) {
  const char *source = R"(
    declare void @_ZNSt15recursive_mutex4lockEv(i8*)
    declare void @_ZNSt15recursive_mutex6unlockEv(i8*)
    @recursive = global i8 0
    define void @a(i8* %m, i1 %recurse) {
    entry:
      call void @_ZNSt15recursive_mutex4lockEv(i8* %m)
      br i1 %recurse, label %call, label %done
    call:
      call void @b(i8* %m, i1 false)
      br label %done
    done:
      ret void
    }
    define void @b(i8* %m, i1 %recurse) {
    entry:
      br i1 %recurse, label %call, label %done
    call:
      call void @a(i8* %m, i1 false)
      br label %done
    done:
      ret void
    }
    define void @main() {
    entry:
      call void @_ZNSt15recursive_mutex4lockEv(i8* @recursive)
      call void @a(i8* @recursive, i1 true)
      call void @_ZNSt15recursive_mutex6unlockEv(i8* @recursive)
      %after = add i32 1, 2
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  const GlobalVariable *recursive = module->getNamedGlobal("recursive");
  EXPECT_TRUE(lsa.mayHoldLock(after, recursive));
  EXPECT_TRUE(lsa.mustHoldLock(after, recursive));
}

TEST_F(LockSetAnalysisTest, CatchSwitchUnwindRetainsCallerMayLocks) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare void @may_throw()
    declare i32 @__CxxFrameHandler3(...)
    @lock = global i8 0
    define void @callee() personality i32 (...)* @__CxxFrameHandler3 {
    entry:
      invoke void @may_throw() to label %done unwind label %dispatch
    done:
      ret void
    dispatch:
      %cs = catchswitch within none [label %handler] unwind to caller
    handler:
      %pad = catchpad within %cs [i8* null]
      catchret from %pad to label %done
    }
    define void @caller() personality i32 (...)* @__CxxFrameHandler3 {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      invoke void @callee() to label %done unwind label %catch
    done:
      ret void
    catch:
      %cs = catchswitch within none [label %handler] unwind to caller
    handler:
      %pad = catchpad within %cs [i8* null]
      %unwind_marker = add i32 1, 2
      catchret from %pad to label %done
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  LockSetAnalysis lsa(*module);
  lsa.analyze();
  const Instruction *unwind_marker = findInstructionByName(
      *module->getFunction("caller"), "unwind_marker");
  const GlobalVariable *lock = module->getNamedGlobal("lock");
  EXPECT_TRUE(lsa.mayHoldLock(unwind_marker, lock));
}

TEST_F(LockSetAnalysisTest, SummaryOrderUsesCallerMustHeldLocks) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    @a = global i8 0
    @b = global i8 0
    define void @helper(i1 %flag) {
    entry:
      br i1 %flag, label %done, label %lock_b
    lock_b:
      call i32 @pthread_mutex_lock(i8* @b)
      br label %done
    done:
      ret void
    }
    define void @f(i1 %flag) {
    entry:
      br i1 %flag, label %lock_a, label %call
    lock_a:
      call i32 @pthread_mutex_lock(i8* @a)
      br label %call
    call:
      call void @helper(i1 %flag)
      ret void
    }
    define void @g() {
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

