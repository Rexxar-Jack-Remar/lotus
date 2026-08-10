// Copyright (c) 2026 XiangZhang
// SPDX-License-Identifier: MIT

#include "Solvers/SMT/SMTStabilizer/api/stabilizer_api.h"
#include "Solvers/SMT/SMTStabilizer/api/stabilizer_c_api.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

using stabilizer::api::SMTStabilizer;
using stabilizer::api::SMTStabilizerOptions;

std::filesystem::path inputPath(const char *name) {
  return std::filesystem::path(SMT_STABILIZER_TEST_INPUT_DIR) / name;
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file)
    throw std::runtime_error("unable to open test file: " + path.string());
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

TEST(SMTStabilizerTest, OptionsRoundTrip) {
  SMTStabilizerOptions options;
  EXPECT_TRUE(options.get_rewrite());
  EXPECT_TRUE(options.get_context_propagation());
  EXPECT_TRUE(options.get_subgraph_pruning());

  options.set_rewrite(false);
  options.set_context_propagation(false);
  options.set_subgraph_pruning(false);

  EXPECT_FALSE(options.get_rewrite());
  EXPECT_FALSE(options.get_context_propagation());
  EXPECT_FALSE(options.get_subgraph_pruning());
}

TEST(SMTStabilizerTest, ApplyFileAndTextMatch) {
  const auto path = inputPath("rewrite_input.smt2");
  const std::string input = readFile(path);

  SMTStabilizer stabilizer;
  const std::string file_output = stabilizer.apply_file(path.string());
  const std::string text_output = stabilizer.apply_text(input);

  EXPECT_FALSE(file_output.empty());
  EXPECT_EQ(file_output, text_output);
  EXPECT_NE(file_output.find("(check-sat)"), std::string::npos);
  EXPECT_NE(file_output.find("(exit)"), std::string::npos);
}

TEST(SMTStabilizerTest, RewriteToggleSmoke) {
  const std::string input = readFile(inputPath("rewrite_input.smt2"));

  SMTStabilizerOptions rewrite_on;
  rewrite_on.set_rewrite(true);
  const std::string rewritten = SMTStabilizer(rewrite_on).apply_text(input);

  SMTStabilizerOptions rewrite_off;
  rewrite_off.set_rewrite(false);
  const std::string raw = SMTStabilizer(rewrite_off).apply_text(input);

  EXPECT_FALSE(rewritten.empty());
  EXPECT_FALSE(raw.empty());
  EXPECT_NE(rewritten.find("(check-sat)"), std::string::npos);
  EXPECT_NE(raw.find("(check-sat)"), std::string::npos);
}

TEST(SMTStabilizerTest, FlagMatrixSmoke) {
  const std::string input = readFile(inputPath("symmetric_input.smt2"));

  for (int mask = 0; mask < 8; ++mask) {
    SMTStabilizerOptions options;
    options.set_rewrite((mask & 1) != 0);
    options.set_context_propagation((mask & 2) != 0);
    options.set_subgraph_pruning((mask & 4) != 0);

    const std::string output = SMTStabilizer(options).apply_text(input);
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("(set-logic"), std::string::npos);
    EXPECT_NE(output.find("(assert"), std::string::npos);
    EXPECT_NE(output.find("(check-sat)"), std::string::npos);
  }
}

TEST(SMTStabilizerTest, CppErrorPaths) {
  SMTStabilizer stabilizer;
  EXPECT_THROW((void)stabilizer.apply_text(""), std::invalid_argument);
  EXPECT_THROW((void)stabilizer.apply_file("/definitely/not/a/real/file.smt2"),
               std::runtime_error);
}

TEST(SMTStabilizerTest, CApi) {
  stabilizer_options *options = stabilizer_options_create();
  ASSERT_NE(options, nullptr);
  stabilizer_options_set_rewrite(options, true);
  stabilizer_options_set_context_propagation(options, false);
  stabilizer_options_set_subgraph_pruning(options, true);

  EXPECT_TRUE(stabilizer_options_get_rewrite(options));
  EXPECT_FALSE(stabilizer_options_get_context_propagation(options));
  EXPECT_TRUE(stabilizer_options_get_subgraph_pruning(options));

  stabilizer_handle *handle = stabilizer_create(options);
  ASSERT_NE(handle, nullptr);
  EXPECT_TRUE(stabilizer_get_rewrite(handle));
  EXPECT_FALSE(stabilizer_get_context_propagation(handle));
  EXPECT_TRUE(stabilizer_get_subgraph_pruning(handle));

  const auto path = inputPath("rewrite_input.smt2");
  char *output = nullptr;
  EXPECT_EQ(stabilizer_apply_file(handle, path.string().c_str(), &output),
            STABILIZER_STATUS_OK);
  ASSERT_NE(output, nullptr);
  EXPECT_NE(std::string(output).find("(check-sat)"), std::string::npos);
  stabilizer_free_string(output);

  output = nullptr;
  EXPECT_EQ(stabilizer_apply_text(handle, "", &output),
            STABILIZER_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(output, nullptr);
  EXPECT_FALSE(std::string(stabilizer_last_error(handle)).empty());

  stabilizer_destroy(handle);
  stabilizer_options_destroy(options);
}

} // namespace
