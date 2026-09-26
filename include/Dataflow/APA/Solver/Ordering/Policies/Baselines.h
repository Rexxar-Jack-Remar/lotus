#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

namespace elimination::order {

struct MinDegreePolicy final {
  static double score(std::size_t Degree) {
    return static_cast<double>(Degree);
  }
};

struct MinFillPolicy final {
  // Fill depends on edges BETWEEN neighbors; incident-only dirty updates are
  // insufficient, so this baseline must fully refresh candidates each step.
  static constexpr bool FULL_RESCORE = true;
  static double score(double Fill) { return Fill; }
};

struct ReversePostOrderPolicy final {
  static double score(std::size_t Rank) { return static_cast<double>(Rank); }
};

struct ExplicitPolicy final {
  static void validate(const std::vector<std::size_t> &Order, std::size_t N) {
    auto Sorted = Order;
    std::sort(Sorted.begin(), Sorted.end());
    if (Sorted.size() != N) {
      throw std::invalid_argument(
          "APA explicit order must be a complete permutation");
    }
    for (std::size_t I = 0; I < N; ++I) {
      if (Sorted[I] != I) {
        throw std::invalid_argument(
            "APA explicit order contains duplicate or invalid indices");
      }
    }
  }
  static std::vector<std::size_t>
  priorities(const std::vector<std::size_t> &Order) {
    std::vector<std::size_t> Result(Order.size());
    for (std::size_t I = 0; I < Order.size(); ++I)
      Result[Order[I]] = I;
    return Result;
  }
  static double score(std::size_t Position) {
    return static_cast<double>(Position);
  }
};

struct RandomPolicy final {
  static std::vector<std::size_t> priorities(std::size_t N,
                                             std::uint64_t Seed) {
    std::vector<std::size_t> Order(N);
    std::iota(Order.begin(), Order.end(), 0);
    std::mt19937_64 Generator(Seed);
    std::shuffle(Order.begin(), Order.end(), Generator);
    return ExplicitPolicy::priorities(Order);
  }
  static double score(std::size_t Position) {
    return static_cast<double>(Position);
  }
};

} // namespace elimination::order
