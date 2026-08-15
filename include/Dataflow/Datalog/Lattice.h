#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <set>
#include <utility>

namespace lotus::datalog {

template <typename T> class MinLattice {
public:
  MinLattice() = default;
  MinLattice(T value) : value_(std::move(value)) {}

  const T &value() const { return value_; }
  T &value() { return value_; }

  bool joinMut(const MinLattice &other) {
    if (!(other.value_ < value_))
      return false;
    value_ = other.value_;
    return true;
  }

  friend bool operator==(const MinLattice &lhs, const MinLattice &rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const MinLattice &lhs, const MinLattice &rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const MinLattice &lhs, const MinLattice &rhs) {
    return lhs.value_ < rhs.value_;
  }
  friend MinLattice operator+(const MinLattice &lhs, const T &rhs) {
    return MinLattice(lhs.value_ + rhs);
  }
  friend MinLattice operator+(const T &lhs, const MinLattice &rhs) {
    return MinLattice(lhs + rhs.value_);
  }

private:
  T value_{};
};

template <typename T> class MaxLattice {
public:
  MaxLattice() = default;
  MaxLattice(T value) : value_(std::move(value)) {}

  const T &value() const { return value_; }
  T &value() { return value_; }

  bool joinMut(const MaxLattice &other) {
    if (!(value_ < other.value_))
      return false;
    value_ = other.value_;
    return true;
  }

  friend bool operator==(const MaxLattice &lhs, const MaxLattice &rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const MaxLattice &lhs, const MaxLattice &rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const MaxLattice &lhs, const MaxLattice &rhs) {
    return lhs.value_ < rhs.value_;
  }
  friend MaxLattice operator+(const MaxLattice &lhs, const T &rhs) {
    return MaxLattice(lhs.value_ + rhs);
  }
  friend MaxLattice operator+(const T &lhs, const MaxLattice &rhs) {
    return MaxLattice(lhs + rhs.value_);
  }

private:
  T value_{};
};

template <typename T> class SetLattice {
public:
  SetLattice() = default;
  SetLattice(std::set<T> values) : values_(std::move(values)) {}
  SetLattice(std::initializer_list<T> values) : values_(values) {}

  const std::set<T> &values() const { return values_; }

  bool joinMut(const SetLattice &other) {
    const std::size_t old_size = values_.size();
    values_.insert(other.values_.begin(), other.values_.end());
    return values_.size() != old_size;
  }

  friend bool operator==(const SetLattice &lhs, const SetLattice &rhs) {
    return lhs.values_ == rhs.values_;
  }
  friend bool operator!=(const SetLattice &lhs, const SetLattice &rhs) {
    return !(lhs == rhs);
  }

private:
  std::set<T> values_;
};

template <typename T> using min_lattice = MinLattice<T>;
template <typename T> using max_lattice = MaxLattice<T>;
template <typename T> using set_lattice = SetLattice<T>;

} // namespace lotus::datalog

namespace std {

template <typename T> struct hash<lotus::datalog::MinLattice<T>> {
  std::size_t operator()(const lotus::datalog::MinLattice<T> &value) const {
    return std::hash<T>{}(value.value());
  }
};

template <typename T> struct hash<lotus::datalog::MaxLattice<T>> {
  std::size_t operator()(const lotus::datalog::MaxLattice<T> &value) const {
    return std::hash<T>{}(value.value());
  }
};

template <typename T> struct hash<lotus::datalog::SetLattice<T>> {
  std::size_t operator()(const lotus::datalog::SetLattice<T> &value) const {
    std::size_t seed = 0;
    for (const T &element : value.values()) {
      const std::size_t hash_value = std::hash<T>{}(element);
      seed ^= hash_value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

} // namespace std
