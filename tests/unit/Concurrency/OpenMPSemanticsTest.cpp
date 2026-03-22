#include "Analysis/Concurrency/OpenMP/OpenMPSemantics.h"

#include "LLVMHelpers.h"

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

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
