#pragma once

#include <cstdint>
#include <functional>
#include <ostream>

namespace lotus::egraph {

class Id {
public:
  constexpr Id() = default;
  explicit constexpr Id(uint32_t value) : value_(value) {}

  static constexpr Id fromIndex(size_t index) {
    return Id(static_cast<uint32_t>(index));
  }

  constexpr uint32_t value() const { return value_; }
  constexpr size_t index() const { return static_cast<size_t>(value_); }

  constexpr explicit operator size_t() const { return index(); }

  friend constexpr bool operator==(Id lhs, Id rhs) {
    return lhs.value_ == rhs.value_;
  }

  friend constexpr bool operator!=(Id lhs, Id rhs) { return !(lhs == rhs); }

  friend constexpr bool operator<(Id lhs, Id rhs) {
    return lhs.value_ < rhs.value_;
  }

  friend constexpr bool operator<=(Id lhs, Id rhs) { return !(rhs < lhs); }
  friend constexpr bool operator>(Id lhs, Id rhs) { return rhs < lhs; }
  friend constexpr bool operator>=(Id lhs, Id rhs) { return !(lhs < rhs); }

private:
  uint32_t value_ = 0;
};

inline std::ostream &operator<<(std::ostream &os, Id id) {
  os << id.value();
  return os;
}

} // namespace lotus::egraph

template <> struct std::hash<lotus::egraph::Id> {
  size_t operator()(lotus::egraph::Id id) const noexcept {
    return std::hash<uint32_t>{}(id.value());
  }
};
