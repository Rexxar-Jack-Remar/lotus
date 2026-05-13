#include "Checker/Core/CheckerDriver.h"
#include "Checker/Core/CheckerRegistry.h"
#include "Checker/Core/CheckerSpecLoader.h"
#include "Checker/Report/BugReportMgr.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;

namespace {

using lotus::checker::CheckerContext;
using lotus::checker::CheckerDriver;
using lotus::checker::CheckerRegistry;
using lotus::checker::CheckerSpecLoader;

TEST(CheckerCoreTest, ParsesForbiddenCallYamlSpec) {
  const char *yaml = R"(
engine: declarative
rule_kind: forbidden_call
metadata:
  id: forbidden.exec
  title: Forbidden exec
  category: security
message: exec is forbidden
functions: [system]
)";

  CheckerSpecLoader loader;
  auto spec_or = loader.loadFromBuffer(yaml, "memory");
  ASSERT_TRUE(static_cast<bool>(spec_or));
  EXPECT_EQ(spec_or->metadata.id, "forbidden.exec");
  EXPECT_EQ(spec_or->forbidden_call.functions.size(), 1u);
  EXPECT_EQ(spec_or->forbidden_call.functions.front(), "system");
}

TEST(CheckerCoreTest, RunsForbiddenCallAndSourceSinkChecks) {
  CheckerSpecLoader loader;
  auto forbidden_or = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: forbidden_call
metadata:
  id: forbidden.system
  title: Forbidden system
  category: security
message: system should not be used
functions: [system]
)", "forbidden");
  ASSERT_TRUE(static_cast<bool>(forbidden_or));

  auto taint_or = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: source_sink
metadata:
  id: taint.getenv-system
  title: getenv to system
  category: taint
message: tainted getenv reaches system
sources: [getenv]
sinks: [system]
sanitizers: [sanitize_input]
)", "taint");
  ASSERT_TRUE(static_cast<bool>(taint_or));

  CheckerRegistry registry;
  ASSERT_FALSE(static_cast<bool>(registry.registerDeclarative(*forbidden_or)));
  ASSERT_FALSE(static_cast<bool>(registry.registerDeclarative(*taint_or)));

  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, R"(
declare i8* @getenv(i8*)
declare i32 @system(i8*)
declare i8* @sanitize_input(i8*)

define i32 @bad(i8* %name) {
entry:
  %src = call i8* @getenv(i8* %name)
  %ret = call i32 @system(i8* %src)
  ret i32 %ret
}

define i32 @good(i8* %name) {
entry:
  %src = call i8* @getenv(i8* %name)
  %safe = call i8* @sanitize_input(i8* %src)
  %ret = call i32 @system(i8* %safe)
  ret i32 %ret
}
)",
      "CheckerCoreTest");
  ASSERT_NE(module, nullptr);

  CheckerContext checker_context{*module};
  CheckerDriver driver(registry, checker_context);
  auto selection = registry.list();
  auto diagnostics_or = driver.run(selection);
  ASSERT_TRUE(static_cast<bool>(diagnostics_or));

  bool saw_forbidden = false;
  int taint_count = 0;
  for (const auto &diagnostic : *diagnostics_or) {
    if (diagnostic.checker_id == "forbidden.system") {
      saw_forbidden = true;
    }
    if (diagnostic.checker_id == "taint.getenv-system") {
      ++taint_count;
    }
  }

  EXPECT_TRUE(saw_forbidden);
  EXPECT_EQ(taint_count, 1);
}

TEST(CheckerCoreTest, DetectsProtocolViolations) {
  CheckerSpecLoader loader;
  auto protocol_or = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: api_protocol
metadata:
  id: protocol.file
  title: File protocol
  category: api-misuse
message: file protocol violation
acquire: [open_resource]
use: [use_resource]
release: [close_resource]
)", "protocol");
  ASSERT_TRUE(static_cast<bool>(protocol_or));

  CheckerRegistry registry;
  ASSERT_FALSE(static_cast<bool>(registry.registerDeclarative(*protocol_or)));

  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, R"(
declare i8* @open_resource()
declare void @use_resource(i8*)
declare void @close_resource(i8*)

define void @bad_use_before_acquire(i8* %p) {
entry:
  call void @use_resource(i8* %p)
  ret void
}

define void @bad_leak() {
entry:
  %fd = call i8* @open_resource()
  call void @use_resource(i8* %fd)
  ret void
}

define void @good() {
entry:
  %fd = call i8* @open_resource()
  call void @use_resource(i8* %fd)
  call void @close_resource(i8* %fd)
  ret void
}
)",
      "CheckerCoreTest");
  ASSERT_NE(module, nullptr);

  CheckerContext checker_context{*module};
  CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(registry.list());
  ASSERT_TRUE(static_cast<bool>(diagnostics_or));

  int count = 0;
  for (const auto &diagnostic : *diagnostics_or) {
    if (diagnostic.checker_id == "protocol.file") {
      ++count;
    }
  }
  EXPECT_EQ(count, 2);
}

TEST(CheckerCoreTest, EmitsThroughBugReportManager) {
  CheckerSpecLoader loader;
  auto forbidden_or = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: forbidden_call
metadata:
  id: forbidden.system
  title: Forbidden system
  category: security
message: system should not be used
functions: [system]
)", "forbidden");
  ASSERT_TRUE(static_cast<bool>(forbidden_or));

  CheckerRegistry registry;
  ASSERT_FALSE(static_cast<bool>(registry.registerDeclarative(*forbidden_or)));

  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, R"(
declare i32 @system(i8*)
define void @f(i8* %cmd) {
entry:
  call i32 @system(i8* %cmd)
  ret void
}
)",
      "CheckerCoreTest");

  CheckerContext checker_context{*module};
  CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(registry.list());
  ASSERT_TRUE(static_cast<bool>(diagnostics_or));
  ASSERT_FALSE(static_cast<bool>(driver.emitToReportManager(*diagnostics_or)));

  auto &mgr = BugReportMgr::get_instance();
  EXPECT_GT(mgr.get_total_reports(), 0);
  mgr.clear_all_reports();
}

} // namespace
