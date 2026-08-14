#include "OpenMPSemanticsTestSupport.h"

TEST_F(OpenMPSemanticsTest,
       DetachedTaskCompletionDoesNotCreateSyntheticWaitBoundary) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)

    define internal void @detached_body() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
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

  EXPECT_EQ(semantics.getSummary().detach_completion_count, 0u);
  EXPECT_EQ(semantics.getSummary().wait_boundary_count, 0u);
  EXPECT_TRUE(semantics.getWaitBoundaryInfos().empty());
  EXPECT_EQ(semantics.getDeferredReasonCounts().count(
                "omp_detached_fulfillment_event_unresolved"),
            1u);
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
TEST_F(OpenMPSemanticsTest,
       SpecializedAtomicRuntimeFamilyIsReportedExplicitly) {
  const char *source = R"(
    declare i32 @__kmpc_atomic_fixed4_add(i8*, i32, i32*, i32)

    define i32 @main(i32* %address) {
    entry:
      %result = call i32 @__kmpc_atomic_fixed4_add(
          i8* null, i32 0, i32* %address, i32 1)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  EXPECT_EQ(semantics.getSummary().atomic_region_count, 1u);
  auto it =
      semantics.getDeferredReasonCounts().find("omp_atomic_runtime_unmodeled");
  ASSERT_NE(it, semantics.getDeferredReasonCounts().end());
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
       NonDetachedTaskCompletionDoesNotIncrementDetachedCompletionSummary) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)

    define internal void @task_body() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      %task = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 0, i64 32, i64 0, void ()* @task_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task)
      call void @__kmpc_omp_task_complete_if0(i8* null, i32 0, i8* %task)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  EXPECT_EQ(semantics.getSummary().detached_task_count, 0u);
  EXPECT_EQ(semantics.getSummary().detach_completion_count, 0u);
  const auto &reasons = semantics.getDeferredReasonCounts();
  auto unresolved_it = reasons.find("omp_detached_task_completion_unresolved");
  if (unresolved_it != reasons.end()) {
    EXPECT_EQ(unresolved_it->second, 0u);
  }
  EXPECT_TRUE(semantics.getTaskCompletionEvents().empty() ||
              semantics.getTaskCompletionEvents().front().task == nullptr);
}
TEST_F(OpenMPSemanticsTest, MalformedRecognizedTaskCallIsDeferredSafely) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  EXPECT_TRUE(semantics.getTasks().empty());
  auto it =
      semantics.getDeferredReasonCounts().find("omp_task_abi_arity_invalid");
  ASSERT_NE(it, semantics.getDeferredReasonCounts().end());
  EXPECT_EQ(it->second, 1u);
}
TEST_F(OpenMPSemanticsTest, TruncatedTaskWithDepsCallIsDeferredSafely) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  semantics.analyze();

  EXPECT_TRUE(semantics.getTasks().empty());
  auto it =
      semantics.getDeferredReasonCounts().find("omp_task_abi_arity_invalid");
  ASSERT_NE(it, semantics.getDeferredReasonCounts().end());
  EXPECT_EQ(it->second, 1u);
}
