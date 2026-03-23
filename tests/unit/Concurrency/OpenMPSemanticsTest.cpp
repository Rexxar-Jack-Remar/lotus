#include "Analysis/Concurrency/OpenMP/OpenMPSemantics.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;
using namespace OpenMP;

class OpenMPSemanticsTest : public lotus::unittest::LlvmModuleTest {};

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

TEST_F(OpenMPSemanticsTest, AttachesDataSharingFactsToTaskEntities) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @.omp_outlined.(i32* %.omp.shared_ptr, i32 %.omp.val) {
    entry:
      %v = load i32, i32* %.omp.shared_ptr, align 4
      store i32 %v, i32* %.omp.shared_ptr, align 4
      %x = add i32 %.omp.val, 1
      ret void
    }

    define i32 @main() {
    entry:
      %t = call i32 @__kmpc_omp_task(
          i8* null, i32 0,
          i8* bitcast (void (i32*, i32)* @.omp_outlined. to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 1u);
  const Task *task = semantics.getTasks()[0].get();
  ASSERT_NE(task, nullptr);
  ASSERT_EQ(task->data_sharing_entries.size(), 2u);

  bool saw_shared = false;
  bool saw_firstprivate = false;
  for (const DataSharingEntry &entry : task->data_sharing_entries) {
    if (entry.attribute == DataSharingAttribute::Shared) {
      saw_shared = true;
    }
    if (entry.attribute == DataSharingAttribute::Firstprivate) {
      saw_firstprivate = true;
    }
  }
  EXPECT_TRUE(saw_shared);
  EXPECT_TRUE(saw_firstprivate);

  const auto &entity_entries =
      semantics.getDataSharingEntriesForEntity(task->semantic_entity_id);
  EXPECT_EQ(entity_entries.size(), task->data_sharing_entries.size());
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
  EXPECT_EQ(task_events[1].scheduling_context_id, task_events[2].scheduling_context_id);
  EXPECT_EQ(task_events[2].sequence_index, task_events[1].sequence_index + 1);
  EXPECT_EQ(task_events[3].taskgroup_id, task_events[0].taskgroup_id);
}

TEST_F(OpenMPSemanticsTest, NormalizesPartialBoundaryEventsAcrossKinds) {
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
    saw_wait_deps = saw_wait_deps ||
                    event.kind == OpenMPTaskEvent::Kind::TaskwaitDeps;
    saw_flush = saw_flush || event.kind == OpenMPTaskEvent::Kind::Flush;
    saw_doacross =
        saw_doacross || event.kind == OpenMPTaskEvent::Kind::DoacrossWait;
    saw_target =
        saw_target || (event.kind == OpenMPTaskEvent::Kind::TargetBoundary &&
                       event.boundary_kind == WaitBoundaryInfo::Kind::TargetDataNowait);
    saw_reduce = saw_reduce ||
                 event.kind == OpenMPTaskEvent::Kind::ReductionNowaitBoundary;
  }

  EXPECT_EQ(partial_count, 5u);
  EXPECT_TRUE(saw_wait_deps);
  EXPECT_TRUE(saw_flush);
  EXPECT_TRUE(saw_doacross);
  EXPECT_TRUE(saw_target);
  EXPECT_TRUE(saw_reduce);
}

TEST_F(OpenMPSemanticsTest, MasterAndOrderedEndsDoNotBecomeWaitBoundaries) {
  const char *source = R"(
    declare i32 @__kmpc_master(i8*, i32)
    declare void @__kmpc_end_master(i8*, i32)
    declare void @__kmpc_ordered(i8*, i32)
    declare void @__kmpc_end_ordered(i8*, i32)

    define i32 @main() {
    entry:
      %m = call i32 @__kmpc_master(i8* null, i32 0)
      call void @__kmpc_end_master(i8* null, i32 0)
      call void @__kmpc_ordered(i8* null, i32 0)
      call void @__kmpc_end_ordered(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  EXPECT_EQ(semantics.getWaitBoundaryInfos().size(), 0u);
  EXPECT_EQ(semantics.getTaskEvents().size(), 0u);
  EXPECT_EQ(semantics.getSummary().master_region_count, 1u);
  EXPECT_EQ(semantics.getSummary().ordered_region_count, 1u);
}

TEST_F(OpenMPSemanticsTest, MismatchedNestedRegionEndsAreDeferredExplicitly) {
  const char *source = R"(
    declare i32 @__kmpc_master(i8*, i32)
    declare void @__kmpc_end_master(i8*, i32)
    declare void @__kmpc_ordered(i8*, i32)
    declare void @__kmpc_end_ordered(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_master(i8* null, i32 0)
      call void @__kmpc_ordered(i8* null, i32 0)
      call void @__kmpc_end_master(i8* null, i32 0)
      call void @__kmpc_end_ordered(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  const auto &reasons = semantics.getDeferredReasonCounts();
  auto mismatch_it = reasons.find("omp_region_mismatched_end");
  ASSERT_NE(mismatch_it, reasons.end());
  EXPECT_GT(mismatch_it->second, 0u);
  auto unmatched_it = reasons.find("omp_region_end_unmatched");
  ASSERT_NE(unmatched_it, reasons.end());
  EXPECT_GT(unmatched_it->second, 0u);
}

TEST_F(OpenMPSemanticsTest, ValidSectionsAndReduceDoNotTriggerMalformedRegionCounters) {
  const char *source = R"(
    declare i32 @__kmpc_sections_init(i8*, i32)
    declare void @__kmpc_end_sections(i8*, i32)
    declare i32 @__kmpc_reduce(i8*, i32, i32, i64, i8*, i8*, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_sections_init(i8* null, i32 0)
      call void @__kmpc_end_sections(i8* null, i32 0)
      call i32 @__kmpc_reduce(i8* null, i32 0, i32 1, i64 4, i8* null, i8* null, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  const auto &reasons = semantics.getDeferredReasonCounts();
  auto mismatch_it = reasons.find("omp_region_mismatched_end");
  if (mismatch_it != reasons.end()) {
    EXPECT_EQ(mismatch_it->second, 0u);
  }
  auto unmatched_it = reasons.find("omp_region_end_unmatched");
  if (unmatched_it != reasons.end()) {
    EXPECT_EQ(unmatched_it->second, 0u);
  }
}

TEST_F(OpenMPSemanticsTest, AtomicRuntimeFallbackIsReportedExplicitly) {
  const char *source = R"(
    declare void @__kmpc_atomic_start()
    declare void @__kmpc_atomic_end()

    define i32 @main() {
    entry:
      call void @__kmpc_atomic_start()
      call void @__kmpc_atomic_end()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  EXPECT_EQ(semantics.getSummary().atomic_region_count, 1u);
  const auto &reasons = semantics.getDeferredReasonCounts();
  auto it = reasons.find("omp_atomic_runtime_unmodeled");
  ASSERT_NE(it, reasons.end());
  EXPECT_EQ(it->second, 1u);
}

TEST_F(OpenMPSemanticsTest, CancellationRuntimeRemainsExplicitModelGap) {
  const char *source = R"(
    declare i32 @__kmpc_cancel(i8*, i32, i32)
    declare i32 @__kmpc_cancellationpoint(i8*, i32, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_cancel(i8* null, i32 0, i32 0)
      call i32 @__kmpc_cancellationpoint(i8* null, i32 0, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  EXPECT_EQ(semantics.getSummary().cancel_count, 1u);
  EXPECT_EQ(semantics.getSummary().cancellation_point_count, 1u);
  const auto &reasons = semantics.getDeferredReasonCounts();
  auto cancel_it = reasons.find("omp_cancel_runtime_unmodeled");
  ASSERT_NE(cancel_it, reasons.end());
  EXPECT_EQ(cancel_it->second, 1u);
  auto point_it = reasons.find("omp_cancellation_point_runtime_unmodeled");
  ASSERT_NE(point_it, reasons.end());
  EXPECT_EQ(point_it->second, 1u);
}

TEST_F(OpenMPSemanticsTest,
       DoacrossSubmitWitnessesSelectiveWaitRelationsAndTaskCompletionResolves) {
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
  EXPECT_TRUE(saw_task_complete);

  bool saw_detached_completion_relation = false;
  for (const auto &entry : semantics.getRelations()) {
    if (entry.second.reason == "omp_detached_task_completion") {
      saw_detached_completion_relation = true;
    }
  }
  EXPECT_TRUE(saw_detached_completion_relation);
}

TEST_F(OpenMPSemanticsTest,
       TargetDataUpdateDoesNotCreateTaskOrderingBoundary) {
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

TEST_F(OpenMPSemanticsTest, ReusedTaskFunctionGetsDistinctSchedulingContexts) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)

    define internal void @.omp_child_body() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0,
          i8* bitcast (void ()* @.omp_grandchild to i8*))
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      ret void
    }

    define internal void @.omp_grandchild() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0,
          i8* bitcast (void ()* @.omp_child_body to i8*))
      call i32 @__kmpc_omp_task(i8* null, i32 0,
          i8* bitcast (void ()* @.omp_child_body to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 4u);
  std::vector<const Task *> child_tasks;
  std::vector<const Task *> grandchild_tasks;
  for (const auto &task_uptr : semantics.getTasks()) {
    const Task *task = task_uptr.get();
    ASSERT_NE(task, nullptr);
    const Function *task_fn = task->task_function;
    ASSERT_NE(task_fn, nullptr);
    if (task_fn->getName().equals(".omp_child_body")) {
      child_tasks.push_back(task);
    } else if (task_fn->getName().equals(".omp_grandchild")) {
      grandchild_tasks.push_back(task);
    }
  }

  ASSERT_EQ(child_tasks.size(), 2u);
  ASSERT_EQ(grandchild_tasks.size(), 2u);
  EXPECT_NE(grandchild_tasks[0]->scheduling_context_id, 0u);
  EXPECT_NE(grandchild_tasks[1]->scheduling_context_id, 0u);
  EXPECT_NE(grandchild_tasks[0]->scheduling_context_id,
            grandchild_tasks[1]->scheduling_context_id);
  for (const Task *child : child_tasks) {
    for (const Task *grandchild : grandchild_tasks) {
      EXPECT_NE(child->scheduling_context_id, grandchild->scheduling_context_id);
    }
  }

  size_t nested_taskwaits = 0;
  for (const WaitBoundaryInfo &info : semantics.getWaitBoundaryInfos()) {
    if (info.kind == WaitBoundaryInfo::Kind::Taskwait) {
      ++nested_taskwaits;
    }
  }
  EXPECT_EQ(nested_taskwaits, 2u);
}

TEST_F(OpenMPSemanticsTest, TaskAllocFlagsPopulateExecutionModeSummary) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)

    define internal void @untied_body() {
    entry:
      ret void
    }

    define internal void @final_body() {
    entry:
      ret void
    }

    define internal void @detached_body() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      %untied = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 0, i64 32, i64 0, void ()* @untied_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %untied)

      %final = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 3, i64 32, i64 0, void ()* @final_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %final)

      %detached = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 65, i64 32, i64 0, void ()* @detached_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %detached)
      call void @__kmpc_omp_task_complete_if0(i8* null, i32 0, i8* %detached)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  const auto &summary = semantics.getSummary();
  EXPECT_EQ(summary.task_count, 3u);
  EXPECT_EQ(summary.final_task_count, 1u);
  EXPECT_EQ(summary.untied_task_count, 1u);
  EXPECT_EQ(summary.detached_task_count, 1u);
  EXPECT_EQ(summary.detach_completion_count, 1u);

  ASSERT_EQ(semantics.getTasks().size(), 3u);
  bool saw_untied = false;
  bool saw_final = false;
  bool saw_detached = false;
  for (const auto &task_uptr : semantics.getTasks()) {
    const Task *task = task_uptr.get();
    ASSERT_NE(task, nullptr);
    ASSERT_NE(task->task_function, nullptr);
    if (task->task_function->getName().equals("untied_body")) {
      saw_untied = task->is_untied;
    } else if (task->task_function->getName().equals("final_body")) {
      saw_final = task->is_final;
    } else if (task->task_function->getName().equals("detached_body")) {
      saw_detached = task->is_detached;
    }
  }
  EXPECT_TRUE(saw_untied);
  EXPECT_TRUE(saw_final);
  EXPECT_TRUE(saw_detached);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
