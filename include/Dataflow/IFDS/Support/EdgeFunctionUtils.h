/*
 * Reusable IDE edge-function utilities.
 *
 * IDEProblem exposes this wrapper as its edge-function type. std::function is
 * only used internally to store callable implementations.
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace ifds {
namespace edge {

enum class EdgeFunctionKind {
  Identity,
  Constant,
  AllTop,
  AllBottom,
  Join,
  Lambda,
};

template <typename Value> class EdgeFunction {
public:
  using Function = std::function<Value(const Value &)>;

  EdgeFunction() : m_kind(EdgeFunctionKind::Identity), m_name("identity") {
    m_function = [](const Value &value) { return value; };
  }

  template <typename Function,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<Function>, EdgeFunction>>>
  EdgeFunction(Function &&function)
      : m_kind(EdgeFunctionKind::Lambda),
        m_function(std::forward<Function>(function)) {}

  EdgeFunction(EdgeFunctionKind kind, std::string name, Function function,
               std::optional<Value> constant = std::nullopt,
               std::shared_ptr<const EdgeFunction> left = nullptr,
               std::shared_ptr<const EdgeFunction> right = nullptr)
      : m_kind(kind), m_name(std::move(name)), m_function(std::move(function)),
        m_constant(std::move(constant)), m_left(std::move(left)),
        m_right(std::move(right)) {}

  Value operator()(const Value &value) const { return m_function(value); }

  EdgeFunctionKind kind() const { return m_kind; }
  const std::string &name() const { return m_name; }
  const std::optional<Value> &constant() const { return m_constant; }
  const std::shared_ptr<const EdgeFunction> &left() const { return m_left; }
  const std::shared_ptr<const EdgeFunction> &right() const { return m_right; }

  bool structurally_equal(const EdgeFunction &other) const {
    if (m_kind != other.m_kind) {
      return false;
    }

    switch (m_kind) {
    case EdgeFunctionKind::Identity:
      return true;
    case EdgeFunctionKind::Constant:
    case EdgeFunctionKind::AllTop:
    case EdgeFunctionKind::AllBottom:
      return m_constant == other.m_constant;
    case EdgeFunctionKind::Join:
      return m_left && m_right && other.m_left && other.m_right &&
             m_left->structurally_equal(*other.m_left) &&
             m_right->structurally_equal(*other.m_right);
    case EdgeFunctionKind::Lambda:
      return false;
    }
    return false;
  }

private:
  EdgeFunctionKind m_kind;
  std::string m_name;
  Function m_function;
  std::optional<Value> m_constant;
  std::shared_ptr<const EdgeFunction> m_left;
  std::shared_ptr<const EdgeFunction> m_right;
};

template <typename Value> EdgeFunction<Value> identity() {
  return EdgeFunction<Value>();
}

template <typename Value> EdgeFunction<Value> constant(Value value) {
  Value captured = value;
  return EdgeFunction<Value>(
      EdgeFunctionKind::Constant, "constant",
      [captured](const Value &) { return captured; }, std::move(value));
}

template <typename Value> EdgeFunction<Value> all_top(Value value) {
  Value captured = value;
  return EdgeFunction<Value>(
      EdgeFunctionKind::AllTop, "all-top",
      [captured](const Value &) { return captured; }, std::move(value));
}

template <typename Value> EdgeFunction<Value> all_bottom(Value value) {
  Value captured = value;
  return EdgeFunction<Value>(
      EdgeFunctionKind::AllBottom, "all-bottom",
      [captured](const Value &) { return captured; }, std::move(value));
}

template <typename Value, typename Function>
EdgeFunction<Value> lambda(std::string name, Function &&function) {
  return EdgeFunction<Value>(EdgeFunctionKind::Lambda, std::move(name),
                             std::forward<Function>(function));
}

template <typename Value>
EdgeFunction<Value> compose(const EdgeFunction<Value> &first,
                            const EdgeFunction<Value> &second) {
  if (first.kind() == EdgeFunctionKind::Constant ||
      first.kind() == EdgeFunctionKind::AllTop ||
      first.kind() == EdgeFunctionKind::AllBottom) {
    return first;
  }
  if (first.kind() == EdgeFunctionKind::Identity) {
    return second;
  }
  if (second.kind() == EdgeFunctionKind::Identity) {
    return first;
  }
  return lambda<Value>(first.name() + " o " + second.name(),
                       [first, second](const Value &value) {
                         return first(second(value));
                       });
}

template <typename Value, typename Join>
EdgeFunction<Value> join(const EdgeFunction<Value> &left,
                         const EdgeFunction<Value> &right, Join &&join_values) {
  if (left.structurally_equal(right)) {
    return left;
  }

  if (left.kind() == EdgeFunctionKind::AllTop ||
      right.kind() == EdgeFunctionKind::AllTop) {
    return left.kind() == EdgeFunctionKind::AllTop ? left : right;
  }

  if (left.kind() == EdgeFunctionKind::AllBottom) {
    return right;
  }
  if (right.kind() == EdgeFunctionKind::AllBottom) {
    return left;
  }

  if (left.kind() == EdgeFunctionKind::Constant &&
      right.kind() == EdgeFunctionKind::Constant && left.constant() &&
      right.constant()) {
    return constant<Value>(
        join_values(*left.constant(), *right.constant()));
  }

  return EdgeFunction<Value>(
      EdgeFunctionKind::Join, left.name() + " join " + right.name(),
      [left, right, join_values = std::forward<Join>(join_values)](
          const Value &value) {
        return join_values(left(value), right(value));
      },
      std::nullopt, std::make_shared<EdgeFunction<Value>>(left),
      std::make_shared<EdgeFunction<Value>>(right));
}

} // namespace edge
} // namespace ifds
