#pragma once

/*
 * The Rust `egg` crate exposes a powerful `define_language!` macro.
 * Lotus EGraph does not attempt to reproduce that macro in C++.
 *
 * Define a language by providing a type with:
 * - `using Discriminant = ...`
 * - `const std::vector<Id> &children() const`
 * - `std::vector<Id> &childrenMut()`
 * - `Discriminant discriminant() const`
 * - `bool matches(const T &) const`
 *
 * And a `LanguageOps<T>` specialization with:
 * - `static std::optional<T> fromOp(std::string_view, const std::vector<Id> &)`
 * - `static std::string display(const T &)`
 */
