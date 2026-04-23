#pragma once

#include "Solvers/EGraph/Language.h"
#include "Solvers/EGraph/SExp.h"

#include <type_traits>

namespace lotus::egraph {

template <typename L> class RecExpr {
public:
  using value_type = L;

  RecExpr() = default;
  explicit RecExpr(std::vector<L> items) : items_(std::move(items)) {}

  Id add(const L &node) {
    items_.push_back(node);
    return Id::fromIndex(items_.size() - 1);
  }

  Id root() const {
    if (items_.empty()) {
      throw std::runtime_error("RecExpr has no root");
    }
    return Id::fromIndex(items_.size() - 1);
  }

  size_t size() const { return items_.size(); }
  bool empty() const { return items_.empty(); }

  const L &operator[](Id id) const { return items_.at(id.index()); }
  L &operator[](Id id) { return items_.at(id.index()); }

  const std::vector<L> &items() const { return items_; }
  std::vector<L> &items() { return items_; }

  auto begin() const { return items_.begin(); }
  auto end() const { return items_.end(); }

  std::string toString() const { return toString(root()); }

  std::string toString(Id id) const {
    const auto &node = (*this)[id];
    const auto &children = nodeChildren(node);
    if (children.empty()) {
      return displayNode(node);
    }

    std::vector<std::string> parts;
    parts.reserve(children.size() + 1);
    parts.push_back(displayNode(node));
    for (Id child : children) {
      parts.push_back(toString(child));
    }
    return "(" + joinStrings(parts, " ") + ")";
  }

  static RecExpr parse(std::string_view input) {
    auto sexp = SExp::parse(input);
    RecExpr expr;
    buildFromSExp(expr, sexp);
    return expr;
  }

private:
  static Id buildFromSExp(RecExpr &expr, const SExp &sexp) {
    if (sexp.isAtom()) {
      auto node = LanguageOps<L>::fromOp(sexp.atom(), {});
      if (!node) {
        throw std::runtime_error("Failed to parse atom into language node");
      }
      return expr.add(*node);
    }

    const auto &items = sexp.list();
    if (items.empty()) {
      throw std::runtime_error("Cannot parse empty list as expression");
    }
    if (!items.front().isAtom()) {
      throw std::runtime_error("Expression operator must be an atom");
    }

    std::vector<Id> children;
    children.reserve(items.size() - 1);
    for (size_t i = 1; i < items.size(); ++i) {
      children.push_back(buildFromSExp(expr, items[i]));
    }

    auto node = LanguageOps<L>::fromOp(items.front().atom(), children);
    if (!node) {
      throw std::runtime_error("Failed to parse list into language node");
    }
    return expr.add(*node);
  }

  std::vector<L> items_;
};

template <typename L>
inline std::ostream &operator<<(std::ostream &os, const RecExpr<L> &expr) {
  os << expr.toString();
  return os;
}

} // namespace lotus::egraph
