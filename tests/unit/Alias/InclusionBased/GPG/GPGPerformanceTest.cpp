#include "Alias/InclusionBased/GPG/Analysis.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <memory>
#include <ostream>
#include <string>

#include <gtest/gtest.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace lotus::gpg;

namespace {

struct CorpusCase {
  const char *name;
  const char *file;
};

void PrintTo(const CorpusCase &corpus, std::ostream *stream) {
  *stream << corpus.name << " (" << corpus.file << ')';
}

struct ModuleShape {
  std::size_t instructions = 0;
  std::size_t defined_functions = 0;
  std::size_t analyzed_functions = 0;
  std::size_t globals = 0;

  std::size_t scale() const {
    return std::max<std::size_t>(1,
                                 instructions + analyzed_functions + globals);
  }
};

std::string corpusPath(const char *file) {
  return std::string(CMAKE_SOURCE_DIR) + "/tests/regress/Alias/PTA/" + file;
}

std::unique_ptr<llvm::Module> loadCorpusModule(llvm::LLVMContext &context,
                                               const char *file) {
  llvm::SMDiagnostic diagnostic;
  std::unique_ptr<llvm::Module> module =
      llvm::parseIRFile(corpusPath(file), diagnostic, context);
  if (module)
    return module;

  std::string message;
  llvm::raw_string_ostream stream(message);
  diagnostic.print("GPGPerformanceTest", stream);
  ADD_FAILURE() << "could not parse " << file << ":\n" << stream.str();
  return nullptr;
}

ModuleShape moduleShape(const llvm::Module &module) {
  ModuleShape shape;
  shape.globals = module.global_size();
  for (const llvm::Function &function : module) {
    if (function.isIntrinsic())
      continue;
    ++shape.analyzed_functions;
    if (function.isDeclaration())
      continue;
    ++shape.defined_functions;
    shape.instructions += static_cast<std::size_t>(
        std::distance(llvm::inst_begin(function), llvm::inst_end(function)));
  }
  return shape;
}

void checkGraphAccounting(const llvm::Module &module,
                          const GPGAnalysisEngine &analysis,
                          const AnalysisStats &stats) {
  std::size_t initial_gpbs = 0;
  std::size_t initial_gpus = 0;
  std::size_t optimized_gpbs = 0;
  std::size_t optimized_gpus = 0;

  for (const llvm::Function &function : module) {
    if (function.isIntrinsic())
      continue;

    const GPG *summary = analysis.summary(&function);
    ASSERT_NE(summary, nullptr) << function.getName().str();
    EXPECT_TRUE(summary->validate()) << function.getName().str();
    const GPGStats summary_stats = summary->stats();
    optimized_gpbs += summary_stats.blocks;
    optimized_gpus += summary_stats.gpus;

    if (function.isDeclaration())
      continue;
    const GPG *initial = analysis.initialGPG(&function);
    ASSERT_NE(initial, nullptr) << function.getName().str();
    EXPECT_TRUE(initial->validate()) << function.getName().str();
    const GPGStats initial_stats = initial->stats();
    initial_gpbs += initial_stats.blocks;
    initial_gpus += initial_stats.gpus;
  }

  EXPECT_EQ(stats.initial_gpbs, initial_gpbs);
  EXPECT_EQ(stats.initial_gpus, initial_gpus);
  EXPECT_EQ(stats.optimized_gpbs, optimized_gpbs);
  EXPECT_EQ(stats.optimized_gpus, optimized_gpus);
}

void analyzeCorpusCase(const CorpusCase &corpus, bool enforce_soft_budget) {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module = loadCorpusModule(context, corpus.file);
  ASSERT_NE(module, nullptr);
  const ModuleShape shape = moduleShape(*module);

  const auto start = std::chrono::steady_clock::now();
  GPGAnalysisEngine analysis(*module);
  analysis.run();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
  const AnalysisStats &stats = analysis.result().stats();

  ::testing::Test::RecordProperty("input", corpus.file);
  ::testing::Test::RecordProperty("elapsed_ms", elapsed_ms.count());
  ::testing::Test::RecordProperty("instructions", shape.instructions);
  ::testing::Test::RecordProperty("optimized_gpbs", stats.optimized_gpbs);
  ::testing::Test::RecordProperty("optimized_gpus", stats.optimized_gpus);
  std::cout << "[ GPG PERF ] " << corpus.file << ": " << elapsed_ms.count()
            << " ms, " << shape.instructions << " instructions, "
            << stats.optimized_gpbs << " GPBs, " << stats.optimized_gpus
            << " GPUs\n";

  EXPECT_EQ(stats.functions, shape.defined_functions);
  EXPECT_LE(stats.resolved_indirect_calls, stats.indirect_calls);
  checkGraphAccounting(*module, analysis, stats);

  // These intentionally generous, input-scaled bounds catch accidental state
  // explosion without pinning the test to one machine's execution speed.
  const std::size_t input_scale = shape.scale();
  EXPECT_LE(stats.initial_gpbs, input_scale * 4);
  EXPECT_LE(stats.initial_gpus, input_scale * 8);
  EXPECT_LE(stats.optimized_gpbs, input_scale * 32);
  EXPECT_LE(stats.optimized_gpus, input_scale * 64);

  // CTest supplies the hard termination timeout (60 seconds by default). This
  // softer budget makes a completed-but-pathologically-slow regression useful.
  if (enforce_soft_budget) {
    constexpr auto soft_budget = std::chrono::seconds(30);
    EXPECT_LT(elapsed, soft_budget)
        << corpus.file << " exceeded the generous performance budget";
  }
}

class GPGReferenceCorpusSmokeTest
    : public ::testing::TestWithParam<CorpusCase> {};

} // namespace

TEST(GPGPerformanceRegression, SpecEquakeTerminatesWithoutStateExplosion) {
  analyzeCorpusCase({"Equake", "spec-equake.ll"}, true);
}

TEST_P(GPGReferenceCorpusSmokeTest, AnalysisCompletesAndGraphsRemainBounded) {
  analyzeCorpusCase(GetParam(), false);
}

INSTANTIATE_TEST_SUITE_P(
    ReferenceCorpus, GPGReferenceCorpusSmokeTest,
    ::testing::Values(CorpusCase{"Gap", "spec-gap.ll"},
                      CorpusCase{"Parser", "spec-parser.ll"},
                      CorpusCase{"Mesa", "spec-mesa.ll"},
                      CorpusCase{"Vortex", "spec-vortex.ll"}),
    [](const ::testing::TestParamInfo<CorpusCase> &info) {
      return std::string(info.param.name);
    });
