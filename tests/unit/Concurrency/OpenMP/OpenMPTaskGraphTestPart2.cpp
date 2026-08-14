#include "OpenMPTaskGraphTestSupport.h"

TEST_F(OpenMPTaskGraphTest, NestedTaskgroupBoundaryAdvancesOnlyInnerPhase) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = constant [1 x %kmp_depend_info] [
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
TEST_F(OpenMPTaskGraphTest, TargetDataEndRemainsPartialWithoutTargetTaskModel) {
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
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  EXPECT_TRUE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}
TEST_F(OpenMPTaskGraphTest, ReductionProtocolDoesNotInventTaskWait) {
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
            OpenMPTaskGraph::TaskRelation::Parallel);
  EXPECT_TRUE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.getSummary().reduction_nowait_boundary_count, 0u);
  EXPECT_EQ(graph.getDeferredReasonCounts().count(
                "omp_reduction_protocol_unmodeled"),
            1u);
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

TEST_F(OpenMPTaskGraphTest,
       PossibleSameRangeDoesNotBecomeMustButDefiniteSameRangeDoes) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @possible_object = global i32 0
    @definite_object = global i32 0
    @possible_deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @possible_object to i8*), i64 4, i8 2
      }
    ]
    @definite_deps = constant [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @definite_object to i8*), i64 4, i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %possible = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @possible_deps, i64 0, i64 0
      %definite = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @definite_deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %possible, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %possible, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %definite, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %definite, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 4u);
  ASSERT_EQ(tasks[0]->dependencies.size(), 1u);
  ASSERT_EQ(tasks[2]->dependencies.size(), 1u);
  EXPECT_EQ(tasks[0]->dependencies[0].proof, DependencyProof::Possible);
  EXPECT_EQ(tasks[2]->dependencies[0].proof, DependencyProof::Definite);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  EXPECT_TRUE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
  EXPECT_TRUE(graph.happensBefore(tasks[2].get(), tasks[3].get()));
  EXPECT_FALSE(graph.mayBeParallel(tasks[2].get(), tasks[3].get()));
}

TEST_F(OpenMPTaskGraphTest, RecurrentTaskSiteMayOverlapItself) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define i32 @main() {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      %next = add nuw i32 %i, 1
      %more = icmp ult i32 %next, 2
      br i1 %more, label %loop, label %exit
    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_TRUE(tasks[0]->is_recurrent);
  EXPECT_FALSE(tasks[0]->recurrent_instances_serialized);
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[0].get()),
            OpenMPTaskGraph::TaskRelation::Parallel);
  EXPECT_TRUE(graph.mayBeParallel(tasks[0].get(), tasks[0].get()));
}

TEST_F(OpenMPTaskGraphTest, RecurrentDependenceSerializesTaskSiteInstances) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0
    @deps = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 3 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      %next = add nuw i32 %i, 1
      %more = icmp ult i32 %next, 2
      br i1 %more, label %loop, label %exit
    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_TRUE(tasks[0]->is_recurrent);
  EXPECT_TRUE(tasks[0]->recurrent_instances_serialized);
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[0].get()),
            OpenMPTaskGraph::TaskRelation::HappensBefore);
  EXPECT_FALSE(graph.mayBeParallel(tasks[0].get(), tasks[0].get()));
}

TEST_F(OpenMPTaskGraphTest, TaskgroupWaitsForTaskDescendants) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_taskgroup(i8*, i32)
    declare void @__kmpc_end_taskgroup(i8*, i32)

    define internal void @grandchild_body() {
    entry:
      ret void
    }

    define internal void @parent_body() {
    entry:
      call i32 @__kmpc_omp_task(
          i8* null, i32 0,
          i8* bitcast (void ()* @grandchild_body to i8*))
      ret void
    }

    define internal void @post_body() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task(
          i8* null, i32 0, i8* bitcast (void ()* @parent_body to i8*))
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task(
          i8* null, i32 0, i8* bitcast (void ()* @post_body to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const Task *parent = nullptr;
  const Task *grandchild = nullptr;
  const Task *post = nullptr;
  for (const auto &task : graph.getAllTasks()) {
    ASSERT_NE(task->task_function, nullptr);
    if (task->task_function->getName() == "parent_body") {
      parent = task.get();
    } else if (task->task_function->getName() == "grandchild_body") {
      grandchild = task.get();
    } else if (task->task_function->getName() == "post_body") {
      post = task.get();
    }
  }

  ASSERT_NE(parent, nullptr);
  ASSERT_NE(grandchild, nullptr);
  ASSERT_NE(post, nullptr);
  ASSERT_EQ(parent->taskgroup_ids.size(), 1u);
  EXPECT_EQ(grandchild->taskgroup_ids, parent->taskgroup_ids);
  EXPECT_TRUE(post->taskgroup_ids.empty());
  EXPECT_TRUE(graph.happensBefore(parent, post));
  EXPECT_TRUE(graph.happensBefore(grandchild, post));
  EXPECT_FALSE(graph.mayBeParallel(grandchild, post));
}

TEST_F(OpenMPTaskGraphTest, DependenceFlagsPreserveSetAndAllMemoryKinds) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @a = global i32 0
    @b = global i32 0
    @set_dep = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @a to i8*), i64 4, i8 8 }
    ]
    @all_dep = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* null, i64 0, i8 -128 }
    ]
    @out_dep = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @b to i8*), i64 4, i8 2 }
    ]
    @unknown_dep = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @b to i8*), i64 4, i8 16 }
    ]
    @sentinel_all_dep = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* inttoptr (i64 -1 to i8*), i64 0, i8 0 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %set = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @set_dep, i64 0, i64 0
      %all = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @all_dep, i64 0, i64 0
      %out = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @out_dep, i64 0, i64 0
      %unknown = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @unknown_dep, i64 0, i64 0
      %sentinel_all = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @sentinel_all_dep, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %set, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %set, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %all, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %out, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %unknown, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %sentinel_all, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 6u);
  ASSERT_EQ(tasks[0]->dependencies.size(), 1u);
  ASSERT_EQ(tasks[2]->dependencies.size(), 1u);
  EXPECT_EQ(tasks[0]->dependencies[0].type, DependType::INOUTSET);
  EXPECT_EQ(tasks[2]->dependencies[0].type, DependType::ALL_MEMORY);
  EXPECT_EQ(tasks[4]->dependencies[0].type, DependType::UNKNOWN);
  EXPECT_EQ(tasks[5]->dependencies[0].type, DependType::ALL_MEMORY);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_TRUE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
  EXPECT_TRUE(graph.happensBefore(tasks[2].get(), tasks[3].get()));
}

TEST_F(OpenMPTaskGraphTest, HappensBeforeCycleIsClassifiedUnknown) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
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
  Task *first = tasks[0].get();
  Task *second = tasks[1].get();
  first->successors.insert(second);
  second->successors.insert(first);

  EXPECT_EQ(graph.classifyTaskRelation(first, second),
            OpenMPTaskGraph::TaskRelation::Unknown);
  EXPECT_TRUE(graph.mayBeParallel(first, second));
  EXPECT_EQ(graph.getDeferredReasonCounts().count("omp_hb_cycle"), 1u);
}

TEST_F(OpenMPTaskGraphTest, TaskNoaliasDependencyListIsDecoded) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0
    @deps = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 2 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
          [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 0,
          %kmp_depend_info* null, i32 1, %kmp_depend_info* %dep)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 1u);
  ASSERT_EQ(tasks[0]->dependencies.size(), 1u);
  EXPECT_EQ(tasks[0]->dependencies[0].type, DependType::OUT);
  EXPECT_EQ(tasks[0]->dependencies[0].proof, DependencyProof::Definite);
}

TEST_F(OpenMPTaskGraphTest,
       RecurrentMutexInstancesAreExcludedRatherThanHappensBefore) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  ASSERT_EQ(graph.getAllTasks().size(), 1u);
  Task *task = graph.getAllTasks().front().get();
  task->is_recurrent = true;
  task->recurrent_instances_excluded = true;
  EXPECT_EQ(graph.classifyTaskRelation(task, task),
            OpenMPTaskGraph::TaskRelation::Excluded);
  EXPECT_FALSE(graph.mayBeParallel(task, task));
}

TEST_F(OpenMPTaskGraphTest, IncludedTaskSiteInLoopDoesNotOverlapItself) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)

    define i32 @main() {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      call i32 @__kmpc_omp_task_begin_if0(i8* null, i32 0, i8* null)
      %next = add nuw i32 %i, 1
      %more = icmp ult i32 %next, 2
      br i1 %more, label %loop, label %exit
    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  ASSERT_EQ(graph.getAllTasks().size(), 1u);
  const Task *task = graph.getAllTasks().front().get();
  EXPECT_EQ(task->execution_mode, TaskExecutionMode::Included);
  EXPECT_FALSE(task->is_recurrent);
  EXPECT_FALSE(graph.mayBeParallel(task, task));
}
