#pragma once

#include "Solvers/EGraph/Id.h"
#include "Solvers/EGraph/Util.h"

#include <llvm/ADT/SmallVector.h>

namespace lotus::egraph {

template <typename L> struct LanguageOps;

struct SymbolLangDiscriminant {
  Symbol op;
  size_t arity = 0;

  friend bool operator==(const SymbolLangDiscriminant &lhs,
                         const SymbolLangDiscriminant &rhs) {
    return lhs.op == rhs.op && lhs.arity == rhs.arity;
  }

  friend bool operator!=(const SymbolLangDiscriminant &lhs,
                         const SymbolLangDiscriminant &rhs) {
    return !(lhs == rhs);
  }
};

class SymbolLang {
public:
  using Discriminant = SymbolLangDiscriminant;

  SymbolLang() = default;
  SymbolLang(Symbol op, std::vector<Id> children)
      : op_(std::move(op)), children_(children.begin(), children.end()) {}
  SymbolLang(Symbol op, std::initializer_list<Id> children)
      : op_(std::move(op)), children_(children.begin(), children.end()) {}

  static SymbolLang leaf(Symbol op) { return SymbolLang(std::move(op), {}); }

  const Symbol &op() const { return op_; }
  const llvm::SmallVector<Id, 2> &children() const { return children_; }
  llvm::SmallVector<Id, 2> &childrenMut() { return children_; }

  Discriminant discriminant() const {
    return SymbolLangDiscriminant{op_, children_.size()};
  }

  bool matches(const SymbolLang &other) const {
    return op_ == other.op_ && children_.size() == other.children_.size();
  }

  template <typename F> void forEach(F &&fn) const {
    for (Id id : children_) {
      fn(id);
    }
  }

  template <typename F> void forEachMut(F &&fn) {
    for (Id &id : children_) {
      fn(id);
    }
  }

  template <typename F> SymbolLang mapChildren(F &&fn) const {
    auto copy = *this;
    for (Id &id : copy.children_) {
      id = fn(id);
    }
    return copy;
  }

  bool isLeaf() const { return children_.empty(); }

  friend bool operator==(const SymbolLang &lhs, const SymbolLang &rhs) {
    return lhs.op_ == rhs.op_ && lhs.children_ == rhs.children_;
  }

  friend bool operator!=(const SymbolLang &lhs, const SymbolLang &rhs) {
    return !(lhs == rhs);
  }

  friend bool operator<(const SymbolLang &lhs, const SymbolLang &rhs) {
    return std::tie(lhs.op_, lhs.children_) < std::tie(rhs.op_, rhs.children_);
  }

private:
  Symbol op_;
  llvm::SmallVector<Id, 2> children_;
};

template <> struct LanguageOps<SymbolLang> {
  static std::optional<SymbolLang> fromOp(std::string_view op,
                                          const std::vector<Id> &children) {
    return SymbolLang(Symbol(op), children);
  }

  static std::string display(const SymbolLang &node) {
    return std::string(node.op().view());
  }
};

template <typename L> inline std::string displayNode(const L &node) {
  return LanguageOps<L>::display(node);
}

template <typename L>
inline auto nodeChildren(const L &node) -> const decltype(node.children()) & {
  return node.children();
}

template <typename L>
inline auto nodeChildrenMut(L &node) -> decltype(node.childrenMut()) {
  return node.childrenMut();
}

template <typename L> inline bool nodeIsLeaf(const L &node) {
  return node.children().empty();
}

template <typename L> inline bool nodeMatches(const L &lhs, const L &rhs) {
  return lhs.matches(rhs);
}

} // namespace lotus::egraph

template <> struct std::hash<lotus::egraph::SymbolLangDiscriminant> {
  size_t operator()(
      const lotus::egraph::SymbolLangDiscriminant &value) const noexcept {
    size_t seed = std::hash<lotus::egraph::Symbol>{}(value.op);
    lotus::egraph::hashCombine(seed, value.arity);
    return seed;
  }
};

template <> struct std::hash<lotus::egraph::SymbolLang> {
  size_t operator()(const lotus::egraph::SymbolLang &value) const noexcept {
    size_t seed = std::hash<lotus::egraph::Symbol>{}(value.op());
    for (lotus::egraph::Id child : value.children()) {
      lotus::egraph::hashCombine(seed, child);
    }
    return seed;
  }
};
