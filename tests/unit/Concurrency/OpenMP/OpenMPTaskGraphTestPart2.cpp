#include "OpenMPTaskGraphTestSupport.h"

TEST_F(OpenMPTaskGraphTest, NestedTaskgroupBoundaryAdvancesOnlyInnerPhase) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare void @__kmpc_taskgroup(i8*, i32)
    declare void @__kmpc_end_taskgroup(i8*, i32)
    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 3u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_TRUE(graph.happensBefore(tasks[1].get(), tasks[2].get()));
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[2].get()));
}
TEST_F(OpenMPTaskGraphTest, ConditionalTaskwaitDoesNotCreateDefiniteHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %merge

    then:
      %dep0 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep0, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      br label %merge

    merge:
      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
}
TEST_F(OpenMPTaskGraphTest, ConflictingConditionalTasksStayUnknown) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      %dep0 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep0, i32 0, %kmp_depend_info* null)
      ret i32 0

    right:
      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
}
TEST_F(OpenMPTaskGraphTest, BarrierCreatesDefiniteBoundaryAcrossTasks) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_barrier(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call void @__kmpc_barrier(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.getSummary().barrier_count, 1u);
}
TEST_F(OpenMPTaskGraphTest, DoacrossWaitWithoutWitnessStaysPartialUnknown) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_doacross_wait(i8*, i32, i64*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call void @__kmpc_doacross_wait(i8* null, i32 0, i64* null)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  auto it = graph.getDeferredReasonCounts().find("omp_doacross_partial");
  ASSERT_NE(it, graph.getDeferredReasonCounts().end());
  EXPECT_GT(it->second, 0u);
}
TEST_F(OpenMPTaskGraphTest, TargetDataEndNowaitProducesPartialBoundary) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__tgt_target_data_end_nowait(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__tgt_target_data_end_nowait(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  EXPECT_GT(graph.getSummary().target_nowait_boundary_count, 0u);
}
TEST_F(OpenMPTaskGraphTest, TargetDataEndCreatesDefiniteBoundary) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__tgt_target_data_end(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__tgt_target_data_end(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
}
TEST_F(OpenMPTaskGraphTest, ReduceNowaitEndProducesPartialBoundary) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_reduce_nowait(i8*, i32, i32, i64, i8*, i8*, i8*)
    declare i32 @__kmpc_end_reduce_nowait(i8*, i32, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__kmpc_reduce_nowait(i8* null, i32 0, i32 1, i64 4,
                                     i8* null, i8* null, i8* null)
      call i32 @__kmpc_end_reduce_nowait(i8* null, i32 0, i8* null)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  EXPECT_GT(graph.getSummary().reduction_nowait_boundary_count, 0u);
}
TEST_F(OpenMPTaskGraphTest,
       DependencyConflictClassificationDelegatesToSemantics) {
  const char *source = R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  OpenMPTaskGraph graph(*module);
  semantics.analyze();

  Dependency lhs;
  lhs.type = DependType::OUT;
  lhs.address = nullptr;
  lhs.size = 4;

  Dependency rhs = lhs;

  EXPECT_EQ(graph.classifyDependencyConflict(lhs, rhs),
            semantics.classifyDependencyConflictForTesting(lhs, rhs));
}
