#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::egraph {

using Symbol = std::string;
using Duration = std::chrono::steady_clock::duration;
using Instant = std::chrono::steady_clock::time_point;

inline Instant now() { return std::chrono::steady_clock::now(); }

template <typename T> inline void hashCombine(size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) +
          (seed >> 2);
}

struct PairHash {
  template <typename A, typename B>
  size_t operator()(const std::pair<A, B> &value) const noexcept {
    size_t seed = 0;
    hashCombine(seed, value.first);
    hashCombine(seed, value.second);
    return seed;
  }
};

inline std::string trim(std::string_view input) {
  size_t start = 0;
  while (start < input.size() &&
         std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }

  size_t end = input.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }

  return std::string(input.substr(start, end - start));
}

template <typename Range>
inline std::string joinStrings(const Range &items, std::string_view sep) {
  std::ostringstream oss;
  bool first = true;
  for (const auto &item : items) {
    if (!first) {
      oss << sep;
    }
    first = false;
    oss << item;
  }
  return oss.str();
}

} // namespace lotus::egraph
