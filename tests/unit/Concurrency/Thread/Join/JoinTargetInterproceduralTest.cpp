#include "JoinTargetAnalysisTestSupport.h"

TEST_F(JoinTargetAnalysisTest, WrapperStoreIntoPointerSlotResolvesCallerJoin) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @spawn_into_slot(i8** %slot, i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      store i8* %tid, i8** %slot
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i8*
      %tid = alloca i8
      call void @spawn_into_slot(i8** %slot, i8* %tid)
      %join_tid = load i8*, i8** %slot
      call i32 @pthread_join(i8* %join_tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  EXPECT_TRUE(analysis.isUnambiguousJoin(join_inst));
  EXPECT_EQ(analysis.getPossibleJoinedForks(join_inst).size(), 1u);
  EXPECT_EQ(analysis.getFeasibleJoinedForks(join_inst).size(), 1u);
  EXPECT_NE(analysis.getDefiniteFeasibleJoinedFork(join_inst), nullptr);
}
TEST_F(JoinTargetAnalysisTest, WrapperJoinThroughPointerSlotClearsCallerState) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @join_from_slot(i8** %slot) {
    entry:
      %join_tid = load i8*, i8** %slot
      call i32 @pthread_join(i8* %join_tid, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i8*
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      store i8* %tid, i8** %slot
      call void @join_from_slot(i8** %slot)
      %join_tid2 = load i8*, i8** %slot
      call i32 @pthread_join(i8* %join_tid2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  SmallVector<const Instruction *, 2> joins;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        joins.push_back(&inst);
      }
    }
  }

  ASSERT_EQ(joins.size(), 1u);
  EXPECT_TRUE(analysis.getFeasibleJoinedForks(joins[0]).empty());
  EXPECT_FALSE(analysis.isUnambiguousJoin(joins[0]));
}
TEST_F(JoinTargetAnalysisTest, WrapperFieldStoreKeepsDistinctFieldsSeparated) {
  const char *source = R"(
    %pair = type { i8*, i8* }

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @store_into_first(%pair* %slots, i8* %tid, i8* (i8*)* %fn) {
    entry:
      %field0 = getelementptr inbounds %pair, %pair* %slots, i32 0, i32 0
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* %fn, i8* null)
      store i8* %tid, i8** %field0
      ret void
    }

    define void @store_into_second(%pair* %slots, i8* %tid, i8* (i8*)* %fn) {
    entry:
      %field1 = getelementptr inbounds %pair, %pair* %slots, i32 0, i32 1
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* %fn, i8* null)
      store i8* %tid, i8** %field1
      ret void
    }

    define i32 @main() {
    entry:
      %slots = alloca %pair
      %tid1 = alloca i8
      %tid2 = alloca i8
      call void @store_into_first(%pair* %slots, i8* %tid1, i8* (i8*)* @worker1)
      call void @store_into_second(%pair* %slots, i8* %tid2, i8* (i8*)* @worker2)
      %field0 = getelementptr inbounds %pair, %pair* %slots, i32 0, i32 0
      %join_tid = load i8*, i8** %field0
      call i32 @pthread_join(i8* %join_tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  EXPECT_TRUE(analysis.isUnambiguousJoin(join_inst));
  EXPECT_EQ(analysis.getPossibleJoinedForks(join_inst).size(), 1u);
  EXPECT_EQ(analysis.getFeasibleJoinedForks(join_inst).size(), 1u);
  EXPECT_NE(analysis.getDefiniteFeasibleJoinedFork(join_inst), nullptr);
}
TEST_F(JoinTargetAnalysisTest,
       JoinResolutionMarksRepeatedForkSiteAsAmbiguous) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      br i1 %cond, label %left, label %right

    left:
      call void @spawn_helper(i8* %tid)
      br label %join

    right:
      call void @spawn_helper(i8* %tid)
      br label %join

    join:
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  JoinResolution resolution = analysis.getJoinResolution(join_inst);
  EXPECT_FALSE(resolution.unambiguous);
  ASSERT_EQ(resolution.feasible_instances.size(), 1u);
  EXPECT_EQ(resolution.feasible_instances.front().execution_class,
            ThreadExecutionClass::RepeatedExecution);
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::RepeatedForkSite),
            resolution.ambiguity_reasons.end());
}
TEST_F(JoinTargetAnalysisTest, JoinResolutionCarriesPathAlternativesForSelect) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      %join_tid = select i1 %cond, i8* %tid1, i8* %tid2
      call i32 @pthread_join(i8* %join_tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  JoinResolution resolution = analysis.getJoinResolution(join_inst);
  EXPECT_TRUE(resolution.is_path_sensitive);
  EXPECT_EQ(resolution.path_alternatives.size(), 2u);
  EXPECT_EQ(resolution.feasible_instances.size(), 2u);
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::PathMergedAlternatives),
            resolution.ambiguity_reasons.end());
}
TEST_F(JoinTargetAnalysisTest,
       JoinResolutionReportsNoFeasibleInstanceAfterDetach) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_detach(i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_detach(i8* %tid)
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  JoinResolution resolution = analysis.getJoinResolution(join_inst);
  EXPECT_TRUE(resolution.feasible_instances.empty());
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::NoFeasibleInstance),
            resolution.ambiguity_reasons.end());
}

TEST_F(JoinTargetAnalysisTest,
       UnknownIndexWritePreservesConstantSlotAlternatives) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker0(i8* %arg) { ret i8* null }
    define i8* @worker1(i8* %arg) { ret i8* null }
    define i8* @worker2(i8* %arg) { ret i8* null }

    define i32 @main(i1 %cond) {
    entry:
      %arr = alloca [2 x i8]
      %p0 = getelementptr [2 x i8], [2 x i8]* %arr, i64 0, i64 0
      %p1 = getelementptr [2 x i8], [2 x i8]* %arr, i64 0, i64 1
      call i32 @pthread_create(i8* %p0, i8* null, i8* (i8*)* @worker0, i8* null)
      call i32 @pthread_create(i8* %p1, i8* null, i8* (i8*)* @worker1, i8* null)
      %idx = zext i1 %cond to i64
      %px = getelementptr [2 x i8], [2 x i8]* %arr, i64 0, i64 %idx
      call i32 @pthread_create(i8* %px, i8* null, i8* (i8*)* @worker2, i8* null)
      call i32 @pthread_join(i8* %p0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 1u);
  EXPECT_FALSE(analysis.isUnambiguousJoin(joins[0]));
  EXPECT_EQ(analysis.getFeasibleJoinedForks(joins[0]).size(), 2u);
  EXPECT_EQ(analysis.getDefiniteFeasibleJoinedFork(joins[0]), nullptr);
}

TEST_F(JoinTargetAnalysisTest, SelectWriteWeaklyUpdatesEachDestination) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    define i8* @old_worker(i8* %arg) { ret i8* null }
    define i8* @new_worker(i8* %arg) { ret i8* null }

    define i32 @main(i1 %cond) {
    entry:
      %a = alloca i8
      %b = alloca i8
      call i32 @pthread_create(i8* %a, i8* null, i8* (i8*)* @old_worker, i8* null)
      %dst = select i1 %cond, i8* %a, i8* %b
      call i32 @pthread_create(i8* %dst, i8* null, i8* (i8*)* @new_worker, i8* null)
      call i32 @pthread_join(i8* %a, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 1u);
  EXPECT_FALSE(analysis.isUnambiguousJoin(joins[0]));
  EXPECT_EQ(analysis.getFeasibleJoinedForks(joins[0]).size(), 2u);
}

TEST_F(JoinTargetAnalysisTest, EquivalentZeroOffsetGepsShareOneLocation) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    define i8* @worker0(i8* %arg) { ret i8* null }
    define i8* @worker1(i8* %arg) { ret i8* null }

    define i32 @main() {
    entry:
      %obj = alloca [1 x i8]
      %p = getelementptr [1 x i8], [1 x i8]* %obj, i64 0, i64 0
      call i32 @pthread_create(i8* %p, i8* null, i8* (i8*)* @worker0, i8* null)
      %q = getelementptr i8, i8* %p, i64 0
      call i32 @pthread_create(i8* %q, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_join(i8* %p, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto forks = findCallsByName(*module, "main", "pthread_create");
  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(forks.size(), 2u);
  ASSERT_EQ(joins.size(), 1u);
  EXPECT_TRUE(analysis.isUnambiguousJoin(joins[0]));
  EXPECT_EQ(analysis.getDefiniteFeasibleJoinedFork(joins[0]), forks[1]);
}

TEST_F(JoinTargetAnalysisTest, UnknownCallOnAggregateInvalidatesChildHandle) {
  const char *source = R"(
    %slot = type { i8 }
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare void @opaque(%slot*)
    define i8* @worker(i8* %arg) { ret i8* null }

    define i32 @main() {
    entry:
      %s = alloca %slot
      %tid = getelementptr %slot, %slot* %s, i64 0, i32 0
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call void @opaque(%slot* %s)
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 1u);
  JoinResolution resolution = analysis.getJoinResolution(joins[0]);
  EXPECT_FALSE(resolution.unambiguous);
  EXPECT_TRUE(resolution.has_unknown_live_instance);
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::UnknownExternalEffect),
            resolution.ambiguity_reasons.end());
}

TEST_F(JoinTargetAnalysisTest, ReadonlyUnknownCallPreservesChildHandle) {
  const char *source = R"(
    %slot = type { i8 }
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare void @observe(%slot*) readonly
    define i8* @worker(i8* %arg) { ret i8* null }

    define i32 @main() {
    entry:
      %s = alloca %slot
      %tid = getelementptr %slot, %slot* %s, i64 0, i32 0
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call void @observe(%slot* %s)
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 1u);
  EXPECT_TRUE(analysis.isUnambiguousJoin(joins[0]));
  EXPECT_NE(analysis.getDefiniteFeasibleJoinedFork(joins[0]), nullptr);
}

TEST_F(JoinTargetAnalysisTest,
       ConditionalWrapperCreatePreservesCallerHandleAlternative) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    define i8* @old_worker(i8* %arg) { ret i8* null }
    define i8* @new_worker(i8* %arg) { ret i8* null }

    define void @maybe_replace(i8* %out, i1 %cond) {
    entry:
      br i1 %cond, label %replace, label %ret
    replace:
      call i32 @pthread_create(i8* %out, i8* null,
                               i8* (i8*)* @new_worker, i8* null)
      br label %ret
    ret:
      ret void
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @old_worker, i8* null)
      call void @maybe_replace(i8* %tid, i1 %cond)
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 1u);
  EXPECT_FALSE(analysis.isUnambiguousJoin(joins[0]));
  EXPECT_EQ(analysis.getFeasibleJoinedForks(joins[0]).size(), 2u);
}

TEST_F(JoinTargetAnalysisTest, AliasedSummaryOutputsFailClosed) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    define i8* @worker0(i8* %arg) { ret i8* null }
    define i8* @worker1(i8* %arg) { ret i8* null }

    define void @spawn_twice(i8* %a, i8* %b) {
    entry:
      call i32 @pthread_create(i8* %a, i8* null,
                               i8* (i8*)* @worker0, i8* null)
      call i32 @pthread_create(i8* %b, i8* null,
                               i8* (i8*)* @worker1, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call void @spawn_twice(i8* %tid, i8* %tid)
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 1u);
  JoinResolution resolution = analysis.getJoinResolution(joins[0]);
  EXPECT_FALSE(resolution.unambiguous);
  EXPECT_EQ(resolution.feasible_forks.size(), 2u);
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::PathMergedAlternatives),
            resolution.ambiguity_reasons.end());
}

TEST_F(JoinTargetAnalysisTest, WildcardReadWithOneKnownChildIsNotDefinite) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    define i8* @worker(i8* %arg) { ret i8* null }

    define i32 @main(i64 %idx) {
    entry:
      %arr = alloca [2 x i8]
      %p0 = getelementptr [2 x i8], [2 x i8]* %arr, i64 0, i64 0
      call i32 @pthread_create(i8* %p0, i8* null,
                               i8* (i8*)* @worker, i8* null)
      %px = getelementptr [2 x i8], [2 x i8]* %arr, i64 0, i64 %idx
      call i32 @pthread_join(i8* %px, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 1u);
  JoinResolution resolution = analysis.getJoinResolution(joins[0]);
  EXPECT_FALSE(resolution.unambiguous);
  EXPECT_EQ(resolution.feasible_forks.size(), 1u);
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::WildcardLocation),
            resolution.ambiguity_reasons.end());
}

TEST_F(JoinTargetAnalysisTest, AmbiguousJoinKeepsArmFeasibleForLaterJoin) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    define i8* @worker0(i8* %arg) { ret i8* null }
    define i8* @worker1(i8* %arg) { ret i8* null }

    define i32 @main(i1 %cond) {
    entry:
      %a = alloca i8
      %b = alloca i8
      call i32 @pthread_create(i8* %a, i8* null,
                               i8* (i8*)* @worker0, i8* null)
      call i32 @pthread_create(i8* %b, i8* null,
                               i8* (i8*)* @worker1, i8* null)
      %selected = select i1 %cond, i8* %a, i8* %b
      call i32 @pthread_join(i8* %selected, i8* null)
      call i32 @pthread_join(i8* %a, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 2u);
  EXPECT_EQ(analysis.getFeasibleJoinedForks(joins[1]).size(), 1u);
  EXPECT_FALSE(analysis.isUnambiguousJoin(joins[1]));
}

TEST_F(JoinTargetAnalysisTest, DirectWrapperReturnedHandleResolvesJoin) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    define i8* @worker(i8* %arg) { ret i8* null }

    define i8* @spawn() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      ret i8* %tid
    }

    define i32 @main() {
    entry:
      %tid = call i8* @spawn()
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "pthread_join");
  ASSERT_EQ(joins.size(), 1u);
  EXPECT_TRUE(analysis.isUnambiguousJoin(joins[0]));
  EXPECT_NE(analysis.getDefiniteFeasibleJoinedFork(joins[0]), nullptr);
}

TEST_F(JoinTargetAnalysisTest, DeclarationOnlyStdThreadMoveTransfersHandle) {
  const char *source = R"(
    declare void @_ZNSt6threadC1EPFvPvES0_(i8*, i8* (i8*)*, i8*)
    declare void @_ZNSt6threadC1EOS_(i8*, i8*)
    declare void @_ZNSt6thread4joinEv(i8*)
    define i8* @worker(i8* %arg) { ret i8* null }

    define i32 @main() {
    entry:
      %t1 = alloca i8
      %t2 = alloca i8
      call void @_ZNSt6threadC1EPFvPvES0_(i8* %t1,
                                         i8* (i8*)* @worker, i8* null)
      call void @_ZNSt6threadC1EOS_(i8* %t2, i8* %t1)
      call void @_ZNSt6thread4joinEv(i8* %t2)
      call void @_ZNSt6thread4joinEv(i8* %t1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  auto joins = findCallsByName(*module, "main", "_ZNSt6thread4joinEv");
  ASSERT_EQ(joins.size(), 2u);
  EXPECT_TRUE(analysis.isUnambiguousJoin(joins[0]));
  EXPECT_NE(analysis.getDefiniteFeasibleJoinedFork(joins[0]), nullptr);
  EXPECT_TRUE(analysis.getFeasibleJoinedForks(joins[1]).empty());
  EXPECT_FALSE(analysis.isUnambiguousJoin(joins[1]));
}
