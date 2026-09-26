#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace elimination {
namespace detail {

class ScopedNanoseconds final {
public:
  explicit ScopedNanoseconds(std::uint64_t &Total)
      : Total(Total), Start(std::chrono::steady_clock::now()) {}
  ScopedNanoseconds(const ScopedNanoseconds &) = delete;
  ScopedNanoseconds &operator=(const ScopedNanoseconds &) = delete;
  ~ScopedNanoseconds() {
    Total += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - Start)
            .count());
  }

private:
  std::uint64_t &Total;
  std::chrono::steady_clock::time_point Start;
};

// Measures the union of nested intervals, rather than double-counting an
// inner Star evaluation as part of both itself and its enclosing Star.
class ScopedNestedNanoseconds final {
public:
  ScopedNestedNanoseconds(std::uint64_t &Total, std::size_t &Depth)
      : Total(Total), Depth(Depth), Outer(Depth++ == 0),
        Start(std::chrono::steady_clock::now()) {}
  ~ScopedNestedNanoseconds() {
    --Depth;
    if (Outer) {
      Total += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - Start)
              .count());
    }
  }
  ScopedNestedNanoseconds(const ScopedNestedNanoseconds &) = delete;
  ScopedNestedNanoseconds &operator=(const ScopedNestedNanoseconds &) = delete;

private:
  std::uint64_t &Total;
  std::size_t &Depth;
  bool Outer;
  std::chrono::steady_clock::time_point Start;
};

} // namespace detail
} // namespace elimination
