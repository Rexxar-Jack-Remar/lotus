#ifndef NPA_COMMON_H
#define NPA_COMMON_H

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace npa {

using Symbol = std::string;

enum class LinearStrategy {
  Naive,         ///< Vector fixpoint (all vars updated each round)
  Worklist,      ///< Dependency-driven worklist
  SCC,           ///< SCC-based: solve in topological order, fixpoint per SCC
  TensorProduct  ///< Lift to tensor space, solve, project back (TOPLAS 2016)
};

template <class T>
inline void hash_combine(std::size_t &h, const T &v) {
  h ^= std::hash<T>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
}

struct Stat {
  double time{};
  int iters{};
};

/**********************************************************************
 * Domain concept (semiring)
 *********************************************************************/
template <class D>
struct DomainHas {
  template <class T>
  static auto test(int)
      -> decltype(T::zero(), T::one(), T::combine(T::zero(), T::zero()),
                  T::extend(T::zero(), T::zero()),
                  T::extend_lin(T::zero(), T::zero()),
                  T::ndetCombine(T::zero(), T::zero()),
                  T::condCombine(typename T::test_type{}, T::zero(), T::zero()),
                  T::subtract(T::zero(), T::zero()),
                  T::equal(T::zero(), T::zero()), std::true_type{});
  template <class>
  static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D>
using DomVal = typename D::value_type;
template <class D>
using DomTest = typename D::test_type;

#define NPA_REQUIRE_DOMAIN(D)                                                 \
  static_assert(DomainHas<D>::value,                                          \
                "Invalid DOMAIN: missing required methods")

struct Dirty {
  mutable bool dirty_ = true;
  void mark(bool d = true) const { dirty_ = d; }
};

template <class V>
struct Optional {
  bool has{false};
  V val{};
  Optional() = default;
  Optional(const Optional &) = default;
  Optional &operator=(const Optional &) = default;
  Optional &operator=(const V &v_in) {
    val = v_in;
    has = true;
    return *this;
  }
  void reset() { has = false; }
  bool has_value() const { return has; }
  V &operator*() { return val; }
  const V &operator*() const { return val; }
};

} // namespace npa

#endif // NPA_COMMON_H
