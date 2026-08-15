#pragma once

#include "Dataflow/Datalog/Core/Forward.h"
#include "Dataflow/Datalog/Semantic/SemanticIR.h"

#include <algorithm>
#include <any>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace lotus::datalog::detail {

template <typename T> struct IsExpression : std::false_type {};
template <typename T> struct IsExpression<Expr<T>> : std::true_type {};
template <typename T> struct IsExpression<Var<T>> : std::true_type {};

template <typename T, typename = void> struct IsHashable : std::false_type {};

template <typename T>
struct IsHashable<T, std::void_t<decltype(std::declval<std::hash<T> &>()(
                         std::declval<const T &>()))>> : std::true_type {};

template <typename T, typename = void>
struct IsEqualityComparable : std::false_type {};

template <typename T>
struct IsEqualityComparable<T, std::void_t<decltype(std::declval<const T &>() ==
                                                    std::declval<const T &>())>>
    : std::true_type {};

template <typename T, typename = void> struct HasJoinMut : std::false_type {};

template <typename T>
struct HasJoinMut<T, std::void_t<decltype(std::declval<T &>().joinMut(
                         std::declval<const T &>()))>>
    : std::is_same<decltype(std::declval<T &>().joinMut(
                       std::declval<const T &>())),
                   bool> {};

inline std::vector<VarId> mergeReferences(const std::vector<VarId> &lhs,
                                          const std::vector<VarId> &rhs) {
  std::vector<VarId> result = lhs;
  for (VarId id : rhs) {
    if (std::find(result.begin(), result.end(), id) == result.end())
      result.push_back(id);
  }
  return result;
}

Context *mergeContexts(Context *lhs, Context *rhs);

template <typename T> ColumnType makeColumnType() {
  static_assert(IsHashable<T>::value,
                "Datalog relation columns must provide std::hash<T>");
  static_assert(IsEqualityComparable<T>::value,
                "Datalog relation columns must support operator==");

  ColumnType result;
  result.type = typeid(T);
  result.name = typeid(T).name();
  result.hash = [](const std::any &value) {
    return std::hash<T>{}(std::any_cast<const T &>(value));
  };
  result.equal = [](const std::any &lhs, const std::any &rhs) {
    return std::any_cast<const T &>(lhs) == std::any_cast<const T &>(rhs);
  };
  if constexpr (std::is_floating_point_v<T>) {
    result.validate_key = [](const std::any &value) {
      if (std::isnan(std::any_cast<const T &>(value))) {
        throw std::invalid_argument(
            "NaN may not be used in a Datalog relation key");
      }
    };
  }
  return result;
}

} // namespace lotus::datalog::detail
