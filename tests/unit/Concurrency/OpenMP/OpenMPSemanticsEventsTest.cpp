#include "OpenMPSemanticsTestSupport.h"

TEST_F(OpenMPSemanticsTest, NormalizesTasksAndBoundariesIntoSemanticEvents) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 2 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      %t = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      %w = call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 1u);
  ASSERT_EQ(semantics.getWaitBoundaryInfos().size(), 1u);
  EXPECT_GT(semantics.getSemanticEntities().size(), 0u);
  EXPECT_GT(semantics.getSemanticEvents().size(), 0u);
  EXPECT_NE(semantics.getTasks()[0]->semantic_entity_id, 0u);
  EXPECT_NE(semantics.getWaitBoundaryInfos()[0].semantic_entity_id, 0u);
  EXPECT_EQ(semantics.getWaitBoundaryInfos()[0].kind,
            WaitBoundaryInfo::Kind::Taskwait);
}
TEST_F(OpenMPSemanticsTest, ExtractsNormalizedTaskAndBoundaryEvents) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 2 }
    ]

    declare void @__kmpc_taskgroup(i8*, i32)
    declare void @__kmpc_end_taskgroup(i8*, i32)
    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  const auto &task_events = semantics.getTaskEvents();
  ASSERT_EQ(task_events.size(), 4u);
  EXPECT_EQ(task_events[0].kind, OpenMPTaskEvent::Kind::TaskgroupBegin);
  EXPECT_EQ(task_events[1].kind, OpenMPTaskEvent::Kind::TaskCreate);
  EXPECT_EQ(task_events[2].kind, OpenMPTaskEvent::Kind::Taskwait);
  EXPECT_EQ(task_events[3].kind, OpenMPTaskEvent::Kind::TaskgroupEnd);
  EXPECT_EQ(task_events[1].scheduling_context_id,
            task_events[2].scheduling_context_id);
  EXPECT_LT(task_events[1].event_order, task_events[2].event_order);
  EXPECT_EQ(task_events[3].taskgroup_id, task_events[0].taskgroup_id);
}
TEST_F(OpenMPSemanticsTest,
       EventOrderStaysMonotonicAcrossConsecutiveBoundaries) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)
    declare void @__kmpc_barrier(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      call void @__kmpc_barrier(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  const auto &task_events = semantics.getTaskEvents();
  ASSERT_EQ(task_events.size(), 3u);
  EXPECT_LT(task_events[0].event_order, task_events[1].event_order);
  EXPECT_LT(task_events[1].event_order, task_events[2].event_order);
}
TEST_F(OpenMPSemanticsTest,
       BranchLocalTaskwaitIsDeferredInsteadOfCreatingGlobalOrdering) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)

    define i32 @main(i1 %cond) {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      br i1 %cond, label %then, label %else

    then:
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      br label %join

    else:
      br label %join

    join:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 2u);
  ASSERT_EQ(semantics.getWaitBoundaryInfos().size(), 1u);
  EXPECT_TRUE(semantics.getWaitBoundaryInfos().front().is_partial_wait);
  ASSERT_FALSE(semantics.getRelations().empty());
  for (const auto &entry : semantics.getRelations()) {
    EXPECT_EQ(entry.second.kind,
              concurrency::RelationKind::UnknownDueToModelGap);
  }

  const auto &reasons = semantics.getDeferredReasonCounts();
  auto it = reasons.find("omp_cfg_path_sensitive_semantics_deferred");
  ASSERT_NE(it, reasons.end());
  EXPECT_GT(it->second, 0u);
}
TEST_F(OpenMPSemanticsTest, NormalizesOnlyActualPartialBoundaryEvents) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)
    declare i32 @__kmpc_flush(i8*)
    declare void @__kmpc_doacross_wait(i8*, i32, i64*)
    declare i32 @__tgt_target_data_end_nowait(i8*, i32)
    declare i32 @__kmpc_reduce_nowait(i8*, i32, i32, i64, i8*, i8*, i8*)
    declare i32 @__kmpc_end_reduce_nowait(i8*, i32, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 0, i8* null, i32 0, i8* null)
      call i32 @__kmpc_flush(i8* null)
      call void @__kmpc_doacross_wait(i8* null, i32 0, i64* null)
      call i32 @__tgt_target_data_end_nowait(i8* null, i32 0)
      call i32 @__kmpc_reduce_nowait(i8* null, i32 0, i32 1, i64 4, i8* null, i8* null, i8* null)
      call i32 @__kmpc_end_reduce_nowait(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  const auto &task_events = semantics.getTaskEvents();
  size_t partial_count = 0;
  bool saw_wait_deps = false;
  bool saw_flush = false;
  bool saw_doacross = false;
  bool saw_target = false;
  bool saw_reduce = false;
  for (const auto &event : task_events) {
    if (!event.is_partial_wait) {
      continue;
    }
    ++partial_count;
    saw_wait_deps =
        saw_wait_deps || event.kind == OpenMPTaskEvent::Kind::TaskwaitDeps;
    saw_flush = saw_flush || event.kind == OpenMPTaskEvent::Kind::Flush;
    saw_doacross =
        saw_doacross || event.kind == OpenMPTaskEvent::Kind::DoacrossWait;
    saw_target =
        saw_target ||
        (event.kind == OpenMPTaskEvent::Kind::TargetBoundary &&
         event.boundary_kind == WaitBoundaryInfo::Kind::TargetDataNowait);
    saw_reduce = saw_reduce ||
                 event.kind == OpenMPTaskEvent::Kind::ReductionNowaitBoundary;
  }

  EXPECT_EQ(partial_count, 4u);
  EXPECT_TRUE(saw_wait_deps);
  EXPECT_TRUE(saw_flush);
  EXPECT_TRUE(saw_doacross);
  EXPECT_TRUE(saw_target);
  EXPECT_FALSE(saw_reduce);
  EXPECT_EQ(semantics.getDeferredReasonCounts().count(
                "omp_reduction_protocol_unmodeled"),
            1u);
}
TEST_F(OpenMPSemanticsTest,
       DoacrossSubmitWitnessesWaitButIf0CompletionDoesNotFulfillDetach) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)
    declare void @__kmpc_doacross_submit(i8*, i32, i64*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)

    @dep = global i64 0

    define internal void @detached_body() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      %detached = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 65, i64 32, i64 0, void ()* @detached_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %detached)
      call void @__kmpc_doacross_submit(i8* null, i32 0, i64* @dep)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 1, i8* bitcast (i64* @dep to i8*), i32 0, i8* null)
      call void @__kmpc_omp_task_complete_if0(i8* null, i32 0, i8* %detached)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* bitcast (void ()* @detached_body to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 2u);
  bool saw_doacross_submit = false;
  bool saw_task_complete = false;
  for (const OpenMPTaskEvent &event : semantics.getTaskEvents()) {
    if (event.kind == OpenMPTaskEvent::Kind::DoacrossSubmit) {
      saw_doacross_submit = !event.dependencies.empty();
    } else if (event.kind == OpenMPTaskEvent::Kind::TaskComplete) {
      saw_task_complete = event.task != nullptr;
    }
  }
  EXPECT_TRUE(saw_doacross_submit);
  EXPECT_FALSE(saw_task_complete);

  bool saw_detached_completion_relation = false;
  for (const auto &entry : semantics.getRelations()) {
    if (entry.second.reason == "omp_detached_task_completion") {
      saw_detached_completion_relation = true;
    }
  }
  EXPECT_FALSE(saw_detached_completion_relation);
  EXPECT_EQ(semantics.getDeferredReasonCounts().count(
                "omp_detached_fulfillment_event_unresolved"),
            1u);
}
TEST_F(OpenMPSemanticsTest,
       PartialWaitDoesNotInventMustOrderingFromMayConflictFollower) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @slot = global i8* bitcast (i32* @shared to i8*)

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, %kmp_depend_info*, i32,
                                      %kmp_depend_info*)

    define i32 @main() {
    entry:
      %lhsdeps = alloca [1 x %kmp_depend_info], align 8
      %waitdeps = alloca [1 x %kmp_depend_info], align 8
      %rhsdeps = alloca [1 x %kmp_depend_info], align 8
      %slotval = load i8*, i8** @slot, align 8

      %l0 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %lhsdeps, i64 0, i64 0, i32 0
      %l1 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %lhsdeps, i64 0, i64 0, i32 1
      %l2 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %lhsdeps, i64 0, i64 0, i32 2
      store i8* bitcast (i32* @shared to i8*), i8** %l0, align 8
      store i64 4, i64* %l1, align 8
      store i8 2, i8* %l2, align 1

      %w0 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %waitdeps, i64 0, i64 0, i32 0
      %w1 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %waitdeps, i64 0, i64 0, i32 1
      %w2 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %waitdeps, i64 0, i64 0, i32 2
      store i8* bitcast (i32* @shared to i8*), i8** %w0, align 8
      store i64 4, i64* %w1, align 8
      store i8 2, i8* %w2, align 1

      %r0 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %rhsdeps, i64 0, i64 0, i32 0
      %r1 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %rhsdeps, i64 0, i64 0, i32 1
      %r2 = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %rhsdeps, i64 0, i64 0, i32 2
      store i8* %slotval, i8** %r0, align 8
      store i64 4, i64* %r1, align 8
      store i8 2, i8* %r2, align 1

      %lhs = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %lhsdeps, i64 0, i64 0
      %wait = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %waitdeps, i64 0, i64 0
      %rhs = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* %rhsdeps, i64 0, i64 0

      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %lhs, i32 0,
                                          %kmp_depend_info* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 1,
                                     %kmp_depend_info* %wait, i32 0,
                                     %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %rhs, i32 0,
                                          %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 2u);
  ASSERT_EQ(semantics.getRelations().size(), 1u);
  bool saw_selective_hb = false;
  bool saw_unknown_gap = false;
  for (const auto &entry : semantics.getRelations()) {
    saw_selective_hb =
        saw_selective_hb ||
        entry.second.kind == concurrency::RelationKind::SelectiveHappenBefore;
    saw_unknown_gap =
        saw_unknown_gap ||
        entry.second.kind == concurrency::RelationKind::UnknownDueToModelGap;
  }
  EXPECT_FALSE(saw_selective_hb);
  EXPECT_TRUE(saw_unknown_gap);
}
TEST_F(OpenMPSemanticsTest, TargetDataUpdateDoesNotCreateTaskOrderingBoundary) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__tgt_target_data_update(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__tgt_target_data_update(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 2u);
  EXPECT_TRUE(semantics.getWaitBoundaryInfos().empty());
  EXPECT_EQ(semantics.getTaskEvents().size(), 2u);
  EXPECT_TRUE(semantics.getRelations().empty());
}
TEST_F(OpenMPSemanticsTest, TaskwaitDeps51DecodesNowaitAsDependencyTask) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0
    @deps = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 2 }
    ]

    declare void @__kmpc_omp_taskwait_deps_51(
        i8*, i32, i32, %kmp_depend_info*, i32, %kmp_depend_info*, i32)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call void @__kmpc_omp_taskwait_deps_51(
          i8* null, i32 0, i32 1, %kmp_depend_info* %dep,
          i32 0, %kmp_depend_info* null, i32 0)
      call void @__kmpc_omp_taskwait_deps_51(
          i8* null, i32 0, i32 1, %kmp_depend_info* %dep,
          i32 0, %kmp_depend_info* null, i32 1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getWaitBoundaryInfos().size(), 1u);
  ASSERT_EQ(semantics.getTasks().size(), 1u);
  EXPECT_EQ(semantics.getSummary().task_count, 1u);
  EXPECT_EQ(semantics.getSummary().task_with_dependencies_count, 1u);
  const Task *dependency_task = semantics.getTasks().front().get();
  EXPECT_TRUE(dependency_task->has_deferred_submission);
  EXPECT_TRUE(dependency_task->has_dependency_submission);
  ASSERT_EQ(dependency_task->dependencies.size(), 1u);
  EXPECT_EQ(dependency_task->dependencies[0].proof, DependencyProof::Definite);
}
