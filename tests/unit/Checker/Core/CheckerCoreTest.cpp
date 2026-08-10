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

TEST(CheckerCoreTest, PropagatesSourceSinkTaintThroughSimpleMemory) {
  CheckerSpecLoader loader;
  auto taint_or = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: source_sink
metadata:
  id: taint.store-load
  title: Store/load taint
  category: taint
message: tainted source reaches sink
sources: [source]
sinks: [sink]
)", "store-load");
  ASSERT_TRUE(static_cast<bool>(taint_or));

  CheckerRegistry registry;
  ASSERT_FALSE(static_cast<bool>(registry.registerDeclarative(*taint_or)));

  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, R"(
declare i8* @source()
declare void @sink(i8*)

define void @f() {
entry:
  %slot = alloca i8*
  %value = call i8* @source()
  store i8* %value, i8** %slot
  %loaded = load i8*, i8** %slot
  call void @sink(i8* %loaded)
  ret void
}
)",
      "CheckerCoreTest");
  ASSERT_NE(module, nullptr);

  CheckerContext checker_context{*module};
  CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(registry.list());
  ASSERT_TRUE(static_cast<bool>(diagnostics_or));
  ASSERT_EQ(diagnostics_or->size(), 1u);
  EXPECT_EQ(diagnostics_or->front().checker_id, "taint.store-load");
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
acquire:
  - function: open_resource
    resource: return
use:
  - function: use_resource
    resource_arg: 0
release:
  - function: close_resource
    resource_arg: 0
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

TEST(CheckerCoreTest, UsesConfiguredProtocolResourceArguments) {
  CheckerSpecLoader loader;
  auto protocol_or = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: api_protocol
metadata:
  id: protocol.file
  title: File protocol
  category: api-misuse
message: file protocol violation
acquire:
  - function: fopen
    resource: return
use:
  - function: fread
    resource_arg: 3
release:
  - function: fclose
    resource_arg: 0
)", "protocol-resource-arg");
  ASSERT_TRUE(static_cast<bool>(protocol_or));

  CheckerRegistry registry;
  ASSERT_FALSE(static_cast<bool>(registry.registerDeclarative(*protocol_or)));

  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, R"(
declare i8* @fopen(i8*, i8*)
declare i64 @fread(i8*, i64, i64, i8*)
declare i32 @fclose(i8*)

define void @f(i8* %path, i8* %mode, i8* %buffer) {
entry:
  %file = call i8* @fopen(i8* %path, i8* %mode)
  %read = call i64 @fread(i8* %buffer, i64 1, i64 16, i8* %file)
  %closed = call i32 @fclose(i8* %file)
  ret void
}
)",
      "CheckerCoreTest");
  ASSERT_NE(module, nullptr);

  CheckerContext checker_context{*module};
  CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(registry.list());
  ASSERT_TRUE(static_cast<bool>(diagnostics_or));
  EXPECT_TRUE(diagnostics_or->empty());
}

TEST(CheckerCoreTest, TracksProtocolStateAlongCfgPaths) {
  CheckerSpecLoader loader;
  auto protocol_or = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: api_protocol
metadata:
  id: protocol.resource
  title: Resource protocol
  category: api-misuse
message: resource protocol violation
acquire:
  - function: open_resource
    resource: return
use:
  - function: use_resource
    resource_arg: 0
release:
  - function: close_resource
    resource_arg: 0
)", "protocol-cfg");
  ASSERT_TRUE(static_cast<bool>(protocol_or));

  CheckerRegistry registry;
  ASSERT_FALSE(static_cast<bool>(registry.registerDeclarative(*protocol_or)));

  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, R"(
declare i8* @open_resource()
declare void @use_resource(i8*)
declare void @close_resource(i8*)

define void @f(i1 %condition) {
entry:
  %resource = call i8* @open_resource()
  br i1 %condition, label %close, label %use

close:
  call void @close_resource(i8* %resource)
  br label %exit

use:
  call void @use_resource(i8* %resource)
  br label %exit

exit:
  ret void
}
)",
      "CheckerCoreTest");
  ASSERT_NE(module, nullptr);

  CheckerContext checker_context{*module};
  CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(registry.list());
  ASSERT_TRUE(static_cast<bool>(diagnostics_or));

  int use_after_release = 0;
  int leaks = 0;
  for (const lotus::checker::CheckerDiagnostic &diagnostic : *diagnostics_or) {
    const auto violation = diagnostic.metadata.find("protocol_violation");
    ASSERT_NE(violation, diagnostic.metadata.end());
    if (violation->second == "use-after-release") {
      ++use_after_release;
    }
    if (violation->second == "leak") {
      ++leaks;
    }
  }
  EXPECT_EQ(use_after_release, 0);
  EXPECT_EQ(leaks, 1);
}

TEST(CheckerCoreTest, RejectsInvalidCheckerEnumsAndConfidence) {
  CheckerSpecLoader loader;
  auto bad_severity = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: forbidden_call
metadata:
  id: bad.severity
  title: Bad severity
  category: security
  severity: critcal
message: invalid
functions: [system]
)", "bad-severity");
  ASSERT_FALSE(static_cast<bool>(bad_severity));
  consumeError(bad_severity.takeError());

  auto bad_confidence = loader.loadFromBuffer(R"(
engine: declarative
rule_kind: forbidden_call
metadata:
  id: bad.confidence
  title: Bad confidence
  category: security
message: invalid
confidence: 101
functions: [system]
)", "bad-confidence");
  ASSERT_FALSE(static_cast<bool>(bad_confidence));
  consumeError(bad_confidence.takeError());
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
