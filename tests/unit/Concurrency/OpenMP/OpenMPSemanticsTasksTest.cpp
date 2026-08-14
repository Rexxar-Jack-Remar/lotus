#include "OpenMPSemanticsTestSupport.h"

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
      EXPECT_NE(child->scheduling_context_id,
                grandchild->scheduling_context_id);
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
  EXPECT_EQ(summary.detach_completion_count, 0u);

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
TEST_F(OpenMPSemanticsTest, ZeroTaskAllocFlagsDoNotMarkTaskAsIncluded) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @task_body() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      %task = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 0, i64 32, i64 0, void ()* @task_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 1u);
  EXPECT_EQ(semantics.getSummary().included_task_count, 0u);
  EXPECT_NE(semantics.getTasks().front()->execution_mode,
            TaskExecutionMode::Included);
}
TEST_F(OpenMPSemanticsTest, If0AndDeferredSubmissionShareOneLogicalTask) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)

    define internal void @task_body() {
    entry:
      ret void
    }

    define i32 @main(i1 %defer) {
    entry:
      %task = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 1, i64 32, i64 0, void ()* @task_body)
      br i1 %defer, label %deferred, label %included
    deferred:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task)
      br label %exit
    included:
      call i32 @__kmpc_omp_task_begin_if0(i8* null, i32 0, i8* %task)
      call void @task_body()
      call void @__kmpc_omp_task_complete_if0(
          i8* null, i32 0, i8* %task)
      br label %exit
    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const CallBase *deferred_call = nullptr;
  const CallBase *included_call = nullptr;
  for (BasicBlock &bb : *module->getFunction("main")) {
    for (Instruction &inst : bb) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call || !call->getCalledFunction()) {
        continue;
      }
      if (call->getCalledFunction()->getName() == "__kmpc_omp_task") {
        deferred_call = call;
      } else if (call->getCalledFunction()->getName() ==
                 "__kmpc_omp_task_begin_if0") {
        included_call = call;
      }
    }
  }
  ASSERT_NE(deferred_call, nullptr);
  ASSERT_NE(included_call, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 1u);
  const Task *task = semantics.getTasks().front().get();
  EXPECT_EQ(semantics.getTaskForCreate(deferred_call), task);
  EXPECT_EQ(semantics.getTaskForCreate(included_call), task);
  EXPECT_TRUE(task->has_deferred_submission);
  EXPECT_TRUE(task->has_included_submission);
  EXPECT_EQ(task->execution_mode, TaskExecutionMode::Deferred);
  EXPECT_EQ(semantics.getSummary().detach_completion_count, 0u);
}
TEST_F(OpenMPSemanticsTest,
       UnmappedPointerCaptureDoesNotBecomeSharedFromPointeeAccess) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @.omp_outlined.(i32* %.omp.ptr) {
    entry:
      store i32 1, i32* %.omp.ptr, align 4
      ret void
    }

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(
          i8* null, i32 0,
          i8* bitcast (void (i32*)* @.omp_outlined. to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  ASSERT_EQ(semantics.getTasks().size(), 1u);
  EXPECT_TRUE(semantics.getTasks().front()->data_sharing_entries.empty());
  EXPECT_EQ(semantics.getDeferredReasonCounts().count(
                "omp_data_sharing_capture_unresolved"),
            1u);
}
TEST_F(OpenMPSemanticsTest,
       TaskFlagsRemainOrthogonalAndFinalStatePropagatesToChildren) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @proxy_body() {
    entry:
      ret void
    }

    define internal void @detachable_body() {
    entry:
      ret void
    }

    define internal void @final_child_body() {
    entry:
      ret void
    }

    define internal void @final_body() {
    entry:
      call i32 @__kmpc_omp_task(
          i8* null, i32 0,
          i8* bitcast (void ()* @final_child_body to i8*))
      ret void
    }

    define i32 @main() {
    entry:
      %proxy = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 17, i64 32, i64 0, void ()* @proxy_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %proxy)
      %detachable = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 65, i64 32, i64 0,
          void ()* @detachable_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %detachable)
      %final = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 2, i64 32, i64 0, void ()* @final_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %final)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  const Task *proxy = nullptr;
  const Task *detachable = nullptr;
  const Task *final_task = nullptr;
  const Task *final_child = nullptr;
  for (const auto &task : semantics.getTasks()) {
    ASSERT_NE(task->task_function, nullptr);
    StringRef name = task->task_function->getName();
    if (name == "proxy_body") {
      proxy = task.get();
    } else if (name == "detachable_body") {
      detachable = task.get();
    } else if (name == "final_body") {
      final_task = task.get();
    } else if (name == "final_child_body") {
      final_child = task.get();
    }
  }

  ASSERT_NE(proxy, nullptr);
  ASSERT_NE(detachable, nullptr);
  ASSERT_NE(final_task, nullptr);
  ASSERT_NE(final_child, nullptr);
  EXPECT_TRUE(proxy->is_proxy);
  EXPECT_FALSE(proxy->is_detached);
  EXPECT_FALSE(detachable->is_proxy);
  EXPECT_TRUE(detachable->is_detached);
  EXPECT_TRUE(final_task->is_final);
  EXPECT_FALSE(final_task->is_untied);
  EXPECT_TRUE(final_child->is_final);
  EXPECT_FALSE(final_child->is_untied);
  EXPECT_EQ(final_child->execution_mode, TaskExecutionMode::Included);
}
