#include "Concurrency/CUDA/PTXAnalyzer.h"

#include <algorithm>
#include <string>

#include <gtest/gtest.h>

using namespace concurrency::cuda::ptx;

namespace {

AnalysisReport analyze(const char *source, KernelConfig config) {
  ModuleAst module = parseModule(source);
  return analyzeKernel(module, config);
}

bool hasDiagnostic(const AnalysisReport &report, const std::string &code) {
  return std::any_of(
      report.diagnostics.begin(), report.diagnostics.end(),
      [&](const Diagnostic &diagnostic) { return diagnostic.code == code; });
}

} // namespace

TEST(PTXAnalyzerTest, ParsesAndLowersSingleEntryKernel) {
  ModuleAst module = parseModule(R"(
    .version 9.2
    .shared .b32 s[2];
    .entry k(.param .u64 out) {
      .reg .u32 %r;
    L0:
      mov.u32 %r, %tid.x;
      ret;
    }
  )");

  ASSERT_EQ(module.version, "9.2");
  ASSERT_EQ(module.entries.size(), 1u);
  EXPECT_EQ(module.entries.front().name, "k");
  ASSERT_EQ(module.declarations.size(), 1u);
  EXPECT_EQ(module.declarations.front().name, "s");
  EXPECT_EQ(module.declarations.front().bytes, 8u);

  KernelIr ir = lowerKernel(module, std::nullopt);
  EXPECT_EQ(ir.name, "k");
  ASSERT_EQ(ir.instructions.size(), 2u);
  EXPECT_EQ(ir.labels.at("L0"), 0u);
  EXPECT_EQ(ir.shared.at("s"), 8u);
}

TEST(PTXAnalyzerTest, DetectsSharedWriteRace) {
  KernelConfig config;
  config.block_dim = {{2, 1, 1}};
  config.pointer_extents["s"] = 4;

  AnalysisReport report = analyze(R"(
    .version 9.2
    .entry k() {
      st.shared.u32 [s+0], %tid.x;
      ret;
    }
  )",
                                  config);

  EXPECT_EQ(report.status, ReportStatus::Unsafe);
  EXPECT_TRUE(hasDiagnostic(report, "data-race"));
}

TEST(PTXAnalyzerTest, BarrierPreventsSharedWriteRace) {
  KernelConfig config;
  config.block_dim = {{2, 1, 1}};
  config.pointer_extents["s"] = 4;

  AnalysisReport report = analyze(R"(
    .version 9.2
    .entry k() {
      setp.eq.u32 %p, %tid.x, 0;
      @%p st.shared.u32 [s+0], %tid.x;
      bar.sync 0;
      setp.eq.u32 %q, %tid.x, 1;
      @%q st.shared.u32 [s+0], %tid.x;
      ret;
    }
  )",
                                  config);

  EXPECT_FALSE(hasDiagnostic(report, "data-race"));
}

TEST(PTXAnalyzerTest, DetectsSharedReadThenWriteRace) {
  KernelConfig config;
  config.block_dim = {{2, 1, 1}};
  config.pointer_extents["s"] = 4;

  AnalysisReport report = analyze(R"(
    .version 9.2
    .entry k() {
      setp.eq.u32 %p, %tid.x, 0;
      @%p ld.shared.u32 %r, [s+0];
      setp.eq.u32 %q, %tid.x, 1;
      @%q st.shared.u32 [s+0], %tid.x;
      ret;
    }
  )",
                                  config);

  EXPECT_TRUE(hasDiagnostic(report, "data-race"));
}

TEST(PTXAnalyzerTest, DetectsOverlappingSharedWriteRace) {
  KernelConfig config;
  config.block_dim = {{2, 1, 1}};
  config.pointer_extents["s"] = 8;

  AnalysisReport report = analyze(R"(
    .version 9.2
    .entry k() {
      setp.eq.u32 %p, %tid.x, 0;
      @%p st.shared.u32 [s+0], %tid.x;
      setp.eq.u32 %q, %tid.x, 1;
      @%q st.shared.u32 [s+2], %tid.x;
      ret;
    }
  )",
                                  config);

  EXPECT_TRUE(hasDiagnostic(report, "data-race"));
}

TEST(PTXAnalyzerTest, ReportsBarrierDeadlock) {
  KernelConfig config;
  config.block_dim = {{2, 1, 1}};

  AnalysisReport report = analyze(R"(
    .version 9.2
    .entry k() {
      setp.eq.u32 %p, %tid.x, 0;
      @%p bar.warp.sync 0x3;
      @!%p bar.warp.sync 0x1;
      ret;
    }
  )",
                                  config);

  EXPECT_TRUE(hasDiagnostic(report, "deadlock"));
}

TEST(PTXAnalyzerTest, ReportsUninitializedSharedReadAndOOB) {
  KernelConfig config;
  config.block_dim = {{1, 1, 1}};
  config.pointer_extents["s"] = 4;

  AnalysisReport report = analyze(R"(
    .version 9.2
    .entry k() {
      ld.shared.u32 %r, [s+4];
      ret;
    }
  )",
                                  config);

  EXPECT_TRUE(hasDiagnostic(report, "uninitialized-read"));
  EXPECT_TRUE(hasDiagnostic(report, "out-of-bounds"));
}

TEST(PTXAnalyzerTest, RecordsGlobalFootprints) {
  KernelConfig config;
  config.block_dim = {{2, 1, 1}};
  config.pointer_extents["out"] = 16;

  AnalysisReport report = analyze(R"(
    .version 9.2
    .entry k() {
      ld.param.u64 %out, [out];
      mul.lo.u32 %off, %tid.x, 4;
      st.global.u32 [%out+%off], %tid.x;
      ret;
    }
  )",
                                  config);

  EXPECT_EQ(report.status, ReportStatus::Safe);
  ASSERT_EQ(report.footprints.size(), 2u);
  EXPECT_TRUE(report.footprints[0].is_write);
  EXPECT_EQ(report.footprints[0].base, "out");
  EXPECT_EQ(report.footprints[0].offset, 0);
  EXPECT_EQ(report.footprints[1].offset, 4);
}

TEST(PTXAnalyzerTest, ReportsUnsupportedOpcodeAndJsonStatus) {
  KernelConfig config;
  AnalysisReport report = analyze(R"(
    .version 9.2
    .entry k() {
      atom.shared.add.u32 [s+0], 1;
      ret;
    }
  )",
                                  config);

  EXPECT_EQ(report.status, ReportStatus::Unsupported);
  EXPECT_TRUE(hasDiagnostic(report, "unsupported"));
  EXPECT_NE(report.toJson().find("\"status\":\"unsupported\""),
            std::string::npos);
}

TEST(PTXAnalyzerTest, ParsesConfigText) {
  KernelConfig config = KernelConfig::fromConfigText(R"(
    [kernel]
    entry = "k"
    block_dim = [4, 2, 1]
    block_idx = [3, 0, 0]

    [pointers]
    out = 32
  )");

  ASSERT_TRUE(config.entry);
  EXPECT_EQ(*config.entry, "k");
  EXPECT_EQ(config.threadCount(), 8u);
  EXPECT_EQ(config.block_idx[0], 3u);
  EXPECT_EQ(config.pointer_extents.at("out"), 32u);
}
