#include "OpenMPSemanticsTestSupport.h"

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
TEST_F(OpenMPSemanticsTest,
       ParallelRegionFrameDoesNotLeakIntoLaterCallerEntities) {
  const char *source = R"(
    declare void @__kmpc_fork_call(i8*, i32, void ()*)
    declare void @__kmpc_taskgroup(i8*, i32)
    declare void @__kmpc_end_taskgroup(i8*, i32)

    define void @outlined() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @__kmpc_fork_call(i8* null, i32 0, void ()* @outlined)
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  size_t root_context_id = 0;
  size_t parallel_region_id = 0;
  size_t taskgroup_id = 0;
  for (const SemanticEntity &entity : semantics.getSemanticEntities()) {
    if (entity.kind == SemanticEntityKind::SchedulingContext &&
        entity.function == module->getFunction("main")) {
      root_context_id = entity.id;
    } else if (entity.kind == SemanticEntityKind::ParallelRegion &&
               entity.function == module->getFunction("main")) {
      parallel_region_id = entity.id;
    } else if (entity.kind == SemanticEntityKind::Taskgroup &&
               entity.function == module->getFunction("main")) {
      taskgroup_id = entity.parent_id;
    }
  }

  ASSERT_NE(root_context_id, 0u);
  ASSERT_NE(parallel_region_id, 0u);
  EXPECT_EQ(taskgroup_id, root_context_id);
  EXPECT_NE(taskgroup_id, parallel_region_id);
}
TEST_F(OpenMPSemanticsTest,
       ExplicitGNUParallelEndKeepsRegionOpenUntilBoundary) {
  const char *source = R"(
    declare void @GOMP_parallel(void ()*, i8*, i32, i32)
    declare void @GOMP_parallel_end()
    declare void @__kmpc_taskgroup(i8*, i32)
    declare void @__kmpc_end_taskgroup(i8*, i32)

    define void @worker() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @GOMP_parallel(void ()* @worker, i8* null, i32 1, i32 0)
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      call void @GOMP_parallel_end()
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  size_t root_context_id = 0;
  size_t parallel_region_id = 0;
  std::vector<size_t> taskgroup_parent_ids;
  for (const SemanticEntity &entity : semantics.getSemanticEntities()) {
    if (entity.kind == SemanticEntityKind::SchedulingContext &&
        entity.function == module->getFunction("main")) {
      root_context_id = entity.id;
    } else if (entity.kind == SemanticEntityKind::ParallelRegion &&
               entity.function == module->getFunction("main")) {
      parallel_region_id = entity.id;
    } else if (entity.kind == SemanticEntityKind::Taskgroup &&
               entity.function == module->getFunction("main")) {
      taskgroup_parent_ids.push_back(entity.parent_id);
    }
  }

  ASSERT_NE(root_context_id, 0u);
  ASSERT_NE(parallel_region_id, 0u);
  ASSERT_EQ(taskgroup_parent_ids.size(), 2u);
  EXPECT_EQ(taskgroup_parent_ids[0], parallel_region_id);
  EXPECT_EQ(taskgroup_parent_ids[1], root_context_id);
}
TEST_F(OpenMPSemanticsTest,
       ValidSectionsAndReduceDoNotTriggerMalformedRegionCounters) {
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
TEST_F(OpenMPSemanticsTest,
       SectionsInitCreatesRegionWithoutSyntheticWaitBoundary) {
  const char *source = R"(
    declare i32 @__kmpc_sections_init(i8*, i32)
    declare void @__kmpc_end_sections(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_sections_init(i8* null, i32 0)
      call void @__kmpc_end_sections(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  size_t sections_entities = 0;
  for (const SemanticEntity &entity : semantics.getSemanticEntities()) {
    if (entity.kind == SemanticEntityKind::SectionsRegion) {
      ++sections_entities;
    }
  }

  EXPECT_EQ(semantics.getSummary().sections_region_count, 1u);
  EXPECT_EQ(sections_entities, 1u);
  EXPECT_TRUE(semantics.getWaitBoundaryInfos().empty());
}
TEST_F(OpenMPSemanticsTest, CriticalRegionsCreateSemanticEntitiesAndEvents) {
  const char *source = R"(
    @crit = global [8 x i32] zeroinitializer

    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define i32 @main() {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  EXPECT_EQ(semantics.getSummary().critical_region_count, 1u);

  size_t critical_entities = 0;
  for (const SemanticEntity &entity : semantics.getSemanticEntities()) {
    if (entity.kind == SemanticEntityKind::CriticalRegion) {
      ++critical_entities;
    }
  }
  EXPECT_EQ(critical_entities, 1u);

  size_t critical_events = 0;
  for (const SemanticEvent &event : semantics.getSemanticEvents()) {
    if (event.kind == SemanticEventKind::RegionBegin ||
        event.kind == SemanticEventKind::RegionEnd) {
      ++critical_events;
    }
  }
  EXPECT_GE(critical_events, 2u);
}
