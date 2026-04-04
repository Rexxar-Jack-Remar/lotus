/******************************************************************************
 * Copyright (c) 2023 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Fabian Schiebel and others
 *****************************************************************************/

//===----------------------------------------------------------------------===//
/// @file Nullable.h
/// @brief Nullable type utilities for optional value handling
///
/// This file provides utilities for handling nullable values. It defines the
/// `Nullable` type alias and `unwrapNullable` functions.
///
/// The `Nullable<T>` type alias behaves as follows:
/// - If T is convertible to bool (like `std::optional<T>`), it uses T directly
/// - Otherwise, it wraps T in `std::optional<T>`
///
/// This allows seamless interoperability between Optional types and other
/// nullable-like types.
///
///===----------------------------------------------------------------------===//

#ifndef PHASAR_UTILS_NULLABLE_H
#define PHASAR_UTILS_NULLABLE_H

#include <optional>
#include <type_traits>
#include <utility>

namespace psr {

/// @brief Type alias for nullable values
///
/// If T is convertible to bool, Nullable<T> is just T. Otherwise, it's
/// `std::optional<T>`. This allows writing generic code that accepts both
/// optional types and regular types.
///
/// @tparam T The type to make nullable
template <typename T>
using Nullable = std::conditional_t<std::is_convertible<T, bool>::value, T,
                                    std::optional<T>>;

/// @brief Unwrap a nullable value
///
/// If the value is already a bool-convertible type, returns it unchanged.
/// Otherwise, extracts the value from an optional.
///
/// @tparam T The underlying type
/// @param Val The nullable value to unwrap
/// @return The unwrapped value
template <typename T>
std::enable_if_t<std::is_convertible<T, bool>::value, T &&>
unwrapNullable(T &&Val) noexcept {
  return std::forward<T>(Val);
}
/// @brief Unwrap an optional rvalue
/// @tparam T The underlying type
/// @param Val The optional to unwrap
/// @return The contained value
template <typename T>
std::enable_if_t<!std::is_convertible<T, bool>::value, T>
unwrapNullable(std::optional<T> &&Val) noexcept {
  return *std::move(Val);
}
/// @brief Unwrap an optional const lvalue
/// @tparam T The underlying type
/// @param Val The optional to unwrap
/// @return A const reference to the contained value
template <typename T>
std::enable_if_t<!std::is_convertible<T, bool>::value, const T &>
unwrapNullable(const std::optional<T> &Val) noexcept {
  return *Val;
}
/// @brief Unwrap an optional lvalue
/// @tparam T The underlying type
/// @param Val The optional to unwrap
/// @return A reference to the contained value
template <typename T>
std::enable_if_t<!std::is_convertible<T, bool>::value, T &>
unwrapNullable(std::optional<T> &Val) noexcept {
  return *Val;
}
} // namespace psr

#endif // PHASAR_UTILS_NULLABLE_H
