#include "Solvers/SMT/SMTSampler/NumericalUtils.h"
#include "Solvers/SMT/SMTSampler/SMTSampler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path unique_path(const std::string &suffix) {
  static unsigned sequence = 0;
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("lotus_smt_sampler_" + std::to_string(ticks) + "_" +
          std::to_string(sequence++) + suffix);
}

void write_file(const std::filesystem::path &path, const std::string &contents) {
  std::ofstream output(path);
  ASSERT_TRUE(output.is_open());
  output << contents;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

TEST(SMTSamplerNumericalUtilsTest, RejectsWideNativeBitVectors) {
  EXPECT_THROW(SMTSampler::BVValue(0, 65), std::invalid_argument);
  EXPECT_THROW(SMTSampler::BVRange::unsigned_full(65), std::invalid_argument);
  EXPECT_THROW(SMTSampler::BVRange::signed_full(65), std::invalid_argument);
}

TEST(QuickSamplerTest, RetainsDimacsEmptyClause) {
  const auto input = unique_path(".cnf");
  const auto output = std::filesystem::path(input.string() + ".samples");
  write_file(input, "p cnf 1 1\n0\n");

  lotus::SMTSampler::runQuickSampler(input.string(), 1, 1.0);

  EXPECT_FALSE(std::filesystem::exists(output));
  std::filesystem::remove(input);
}

TEST(QuickSamplerTest, RetriesUnsatisfiableIndependentSeeds) {
  const auto input = unique_path(".cnf");
  const auto output = std::filesystem::path(input.string() + ".samples");
  write_file(input,
             "p cnf 8 8\n"
             "c ind 1 2 3 4 5 6 7 8 0\n"
             "1 0\n2 0\n3 0\n4 0\n5 0\n6 0\n7 0\n8 0\n");

  lotus::SMTSampler::runQuickSampler(input.string(), 1, 1.0);

  ASSERT_TRUE(std::filesystem::exists(output));
  EXPECT_NE(read_file(output).find("11111111"), std::string::npos);
  std::filesystem::remove(input);
  std::filesystem::remove(output);
}

TEST(RegionSamplerTest, KeepsSignedHighBitValues) {
  const auto input = unique_path(".smt2");
  const auto output = std::filesystem::path(input.string() + ".abs.samples");
  write_file(input,
             "(declare-fun x () (_ BitVec 8))\n"
             "(assert (= x #xff))\n"
             "(check-sat)\n");

  lotus::SMTSampler::runRegionSampler(input.string(), 1, 1000.0);

  ASSERT_TRUE(std::filesystem::exists(output));
  EXPECT_NE(read_file(output).find("-1"), std::string::npos);
  std::filesystem::remove(input);
  std::filesystem::remove(output);
}

TEST(RegionSamplerTest, SupportsSigned64BitMinimum) {
  const auto input = unique_path(".smt2");
  const auto output = std::filesystem::path(input.string() + ".abs.samples");
  write_file(input,
             "(declare-fun x () (_ BitVec 64))\n"
             "(assert (= x #x8000000000000000))\n"
             "(check-sat)\n");

  lotus::SMTSampler::runRegionSampler(input.string(), 1, 1000.0);

  ASSERT_TRUE(std::filesystem::exists(output));
  EXPECT_NE(read_file(output).find("-9223372036854775808"),
            std::string::npos);
  std::filesystem::remove(input);
  std::filesystem::remove(output);
}

TEST(RegionSamplerTest, ValidatesBvProjectionExistentially) {
  const auto input = unique_path(".smt2");
  const auto output = std::filesystem::path(input.string() + ".abs.samples");
  write_file(input,
             "(declare-fun x () (_ BitVec 8))\n"
             "(declare-fun b () Bool)\n"
             "(assert (and (= x #x01) b))\n"
             "(check-sat)\n");

  lotus::SMTSampler::runRegionSampler(input.string(), 1, 1000.0);

  ASSERT_TRUE(std::filesystem::exists(output));
  EXPECT_NE(read_file(output).find("1"), std::string::npos);
  std::filesystem::remove(input);
  std::filesystem::remove(output);
}

} // namespace
