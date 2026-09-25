#pragma once

/**
 * \file
 * \brief Solver-independent iteration helper for equation-system façades.
 */

#include "Dataflow/NPA/Core/Expr/Expressions.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace npa {

class InvalidEquationSystemError : public std::logic_error {
public:
  explicit InvalidEquationSystemError(const std::string &message)
      : std::logic_error(message) {}
};

/// A checked symbol table and canonical dense dependency topology.
struct ValidatedEquationSystem {
  std::unordered_map<Symbol, unsigned> symbol_to_index;
  std::vector<std::vector<unsigned>> dependencies;
};

namespace detail {

inline void require_bound_symbol(const Symbol &symbol,
                                 const std::unordered_set<Symbol> &bound) {
  if (bound.find(symbol) == bound.end())
    throw InvalidEquationSystemError("unbound local equation symbol");
}

struct ScopedExpressionKey {
  const void *expression = nullptr;
  std::size_t scope = 0;

  bool operator==(const ScopedExpressionKey &other) const {
    return expression == other.expression && scope == other.scope;
  }
};

struct ScopedExpressionKeyHash {
  std::size_t operator()(const ScopedExpressionKey &key) const {
    const std::size_t pointerHash = std::hash<const void *>{}(key.expression);
    return pointerHash ^ (key.scope + static_cast<std::size_t>(0x9e3779b9) +
                          (pointerHash << 6) + (pointerHash >> 2));
  }
};

using ScopedExpressionSet =
    std::unordered_set<ScopedExpressionKey, ScopedExpressionKeyHash>;

template <class D>
void collect_free_symbols_impl(const E0<D> &expr,
                               const std::unordered_set<Symbol> &bound,
                               std::unordered_set<Symbol> &free,
                               ScopedExpressionSet &visited,
                               std::size_t scope, std::size_t &next_scope) {
  if (!expr)
    throw InvalidEquationSystemError("null polynomial equation expression");
  if (!visited.insert({expr.get(), scope}).second)
    return;

  using K = typename Exp0<D>::K;
  switch (expr->k) {
  case K::Term:
    return;
  case K::Seq:
  case K::Project:
    collect_free_symbols_impl(expr->t, bound, free, visited, scope,
                              next_scope);
    return;
  case K::Mul:
  case K::Cond:
  case K::Ndet:
    collect_free_symbols_impl(expr->t1, bound, free, visited, scope,
                              next_scope);
    collect_free_symbols_impl(expr->t2, bound, free, visited, scope,
                              next_scope);
    return;
  case K::Call:
    free.insert(expr->sym);
    collect_free_symbols_impl(expr->t, bound, free, visited, scope,
                              next_scope);
    return;
  case K::Hole:
    free.insert(expr->sym);
    return;
  case K::Bound:
    require_bound_symbol(expr->sym, bound);
    return;
  case K::Concat:
    if (bound.find(expr->sym) == bound.end())
      free.insert(expr->sym);
    collect_free_symbols_impl(expr->t1, bound, free, visited, scope,
                              next_scope);
    collect_free_symbols_impl(expr->t2, bound, free, visited, scope,
                              next_scope);
    return;
  case K::Star:
  case K::Mu: {
    auto body_bound = bound;
    body_bound.insert(expr->sym);
    const std::size_t body_scope = next_scope++;
    collect_free_symbols_impl(expr->t, body_bound, free, visited, body_scope,
                              next_scope);
    return;
  }
  }
}

template <class D>
void collect_free_symbols(const E0<D> &expr,
                          const std::unordered_set<Symbol> &bound,
                          std::unordered_set<Symbol> &free) {
  ScopedExpressionSet visited;
  std::size_t next_scope = 1;
  collect_free_symbols_impl(expr, bound, free, visited, 0, next_scope);
}

template <class D>
void collect_free_symbols_impl(const E1<D> &expr,
                               const std::unordered_set<Symbol> &bound,
                               std::unordered_set<Symbol> &free,
                               ScopedExpressionSet &visited,
                               std::size_t scope, std::size_t &next_scope) {
  if (!expr)
    throw InvalidEquationSystemError("null linear equation expression");
  if (!visited.insert({expr.get(), scope}).second)
    return;

  using K = typename Exp1<D>::K;
  switch (expr->k) {
  case K::Term:
    return;
  case K::Seq:
  case K::SeqR:
  case K::Project:
    collect_free_symbols_impl(expr->t, bound, free, visited, scope,
                              next_scope);
    return;
  case K::Cond:
  case K::Ndet:
  case K::Add:
  case K::Sub:
    collect_free_symbols_impl(expr->t1, bound, free, visited, scope,
                              next_scope);
    collect_free_symbols_impl(expr->t2, bound, free, visited, scope,
                              next_scope);
    return;
  case K::Call:
    if (bound.find(expr->sym) == bound.end())
      free.insert(expr->sym);
    return;
  case K::Hole:
    free.insert(expr->sym);
    return;
  case K::Bound:
    require_bound_symbol(expr->sym, bound);
    return;
  case K::Concat:
    if (bound.find(expr->sym) == bound.end())
      free.insert(expr->sym);
    collect_free_symbols_impl(expr->t1, bound, free, visited, scope,
                              next_scope);
    collect_free_symbols_impl(expr->t2, bound, free, visited, scope,
                              next_scope);
    return;
  case K::Star:
  case K::Mu: {
    auto body_bound = bound;
    body_bound.insert(expr->sym);
    const std::size_t body_scope = next_scope++;
    collect_free_symbols_impl(expr->t, body_bound, free, visited, body_scope,
                              next_scope);
    return;
  }
  }
}

template <class D>
void collect_free_symbols(const E1<D> &expr,
                          const std::unordered_set<Symbol> &bound,
                          std::unordered_set<Symbol> &free) {
  ScopedExpressionSet visited;
  std::size_t next_scope = 1;
  collect_free_symbols_impl(expr, bound, free, visited, 0, next_scope);
}

template <class D, class E>
ValidatedEquationSystem
validate_equations(const std::vector<std::pair<Symbol, E>> &equations) {
  ValidatedEquationSystem validated;
  validated.symbol_to_index.reserve(equations.size());
  validated.dependencies.resize(equations.size());

  for (std::size_t i = 0; i < equations.size(); ++i) {
    auto inserted = validated.symbol_to_index.emplace(equations[i].first,
                                                      static_cast<unsigned>(i));
    if (!inserted.second)
      throw InvalidEquationSystemError("duplicate equation LHS symbol");
  }

  for (std::size_t i = 0; i < equations.size(); ++i) {
    std::unordered_set<Symbol> free;
    collect_free_symbols(equations[i].second, {}, free);
    auto &dense_dependencies = validated.dependencies[i];
    dense_dependencies.reserve(free.size());
    for (const auto &symbol : free) {
      auto found = validated.symbol_to_index.find(symbol);
      if (found == validated.symbol_to_index.end())
        throw InvalidEquationSystemError("undefined equation symbol");
      dense_dependencies.push_back(found->second);
    }
    std::sort(dense_dependencies.begin(), dense_dependencies.end());
  }
  return validated;
}

} // namespace detail

template <class D>
ValidatedEquationSystem validate_equation_system(
    const std::vector<std::pair<Symbol, E0<D>>> &equations) {
  return detail::validate_equations<D>(equations);
}

template <class D>
ValidatedEquationSystem validate_linear_equation_system(
    const std::vector<std::pair<Symbol, E1<D>>> &equations) {
  return detail::validate_equations<D>(equations);
}

template <class State> struct IterationResult {
  State value;
  int iterations = 0;
  bool stabilized = false;
  double seconds = 0.0;
};

template <class State, class Step, class Equal>
IterationResult<State> iterate_until_stable(State initial, Step step,
                                            Equal equal, int max_iterations,
                                            bool verbose) {
  auto start = std::chrono::high_resolution_clock::now();
  State current = std::move(initial);
  int iteration = 0;
  bool stabilized = false;
  while (max_iterations < 0 || iteration < max_iterations) {
    State next = step(current);
    stabilized = equal(current, next);
    current = std::move(next);
    ++iteration;
    if (stabilized) {
      if (verbose)
        std::cerr << "[conv] " << iteration << "\n";
      break;
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  return {std::move(current), iteration, stabilized,
          std::chrono::duration<double>(end - start).count()};
}

} // namespace npa

