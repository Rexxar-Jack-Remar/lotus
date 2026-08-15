#pragma once

#include "Solvers/EGraph/Language.h"

namespace lotus::egraph {

struct DynamicLangDiscriminant {
  Symbol op;
  size_t arity = 0;

  friend bool operator==(DynamicLangDiscriminant lhs,
                         DynamicLangDiscriminant rhs) {
    return lhs.op == rhs.op && lhs.arity == rhs.arity;
  }

  friend bool operator!=(DynamicLangDiscriminant lhs,
                         DynamicLangDiscriminant rhs) {
    return !(lhs == rhs);
  }
};

class DynamicLang {
public:
  using Discriminant = DynamicLangDiscriminant;

  DynamicLang() = default;
  DynamicLang(Symbol op, std::vector<Id> children)
      : op_(std::move(op)), children_(children.begin(), children.end()) {}
  DynamicLang(Symbol op, std::initializer_list<Id> children)
      : op_(std::move(op)), children_(children.begin(), children.end()) {}

  static DynamicLang leaf(Symbol op) { return DynamicLang(std::move(op), {}); }

  const Symbol &op() const { return op_; }
  const llvm::SmallVector<Id, 2> &children() const { return children_; }
  llvm::SmallVector<Id, 2> &childrenMut() { return children_; }

  Discriminant discriminant() const {
    return Discriminant{op_, children_.size()};
  }

  bool matches(const DynamicLang &other) const {
    return op_ == other.op_ && children_.size() == other.children_.size();
  }

  template <typename F> DynamicLang mapChildren(F &&fn) const {
    auto copy = *this;
    for (Id &id : copy.children_) {
      id = fn(id);
    }
    return copy;
  }

  friend bool operator==(const DynamicLang &lhs, const DynamicLang &rhs) {
    return lhs.op_ == rhs.op_ && lhs.children_ == rhs.children_;
  }

  friend bool operator!=(const DynamicLang &lhs, const DynamicLang &rhs) {
    return !(lhs == rhs);
  }

  friend bool operator<(const DynamicLang &lhs, const DynamicLang &rhs) {
    return std::tie(lhs.op_, lhs.children_) < std::tie(rhs.op_, rhs.children_);
  }

private:
  Symbol op_;
  llvm::SmallVector<Id, 2> children_;
};

template <> struct LanguageOps<DynamicLang> {
  static std::optional<DynamicLang> fromOp(std::string_view op,
                                           const std::vector<Id> &children) {
    return DynamicLang(Symbol(op), children);
  }

  static std::string display(const DynamicLang &node) {
    return std::string(node.op().view());
  }
};

class LanguageBuilder {
public:
  LanguageBuilder &op(Symbol op, size_t arity) {
    entries_.push_back({std::move(op), arity});
    return *this;
  }

  bool allows(Symbol op, size_t arity) const {
    for (const auto &entry : entries_) {
      if (entry.op == op && entry.arity == arity) {
        return true;
      }
    }
    return false;
  }

private:
  struct Entry {
    Symbol op;
    size_t arity = 0;
  };

  std::vector<Entry> entries_;
};

template <typename Tag> struct DefinedLangDiscriminant {
  Symbol op;
  size_t arity = 0;

  friend bool operator==(DefinedLangDiscriminant lhs,
                         DefinedLangDiscriminant rhs) {
    return lhs.op == rhs.op && lhs.arity == rhs.arity;
  }

  friend bool operator!=(DefinedLangDiscriminant lhs,
                         DefinedLangDiscriminant rhs) {
    return !(lhs == rhs);
  }
};

template <typename Tag> class DefinedLang {
public:
  using Discriminant = DefinedLangDiscriminant<Tag>;

  DefinedLang() = default;
  DefinedLang(Symbol op, std::vector<Id> children)
      : op_(std::move(op)), children_(children.begin(), children.end()) {}
  DefinedLang(Symbol op, std::initializer_list<Id> children)
      : op_(std::move(op)), children_(children.begin(), children.end()) {}

  static DefinedLang leaf(Symbol op) { return DefinedLang(std::move(op), {}); }

  const Symbol &op() const { return op_; }
  const llvm::SmallVector<Id, 2> &children() const { return children_; }
  llvm::SmallVector<Id, 2> &childrenMut() { return children_; }

  Discriminant discriminant() const {
    return Discriminant{op_, children_.size()};
  }

  bool matches(const DefinedLang &other) const {
    return op_ == other.op_ && children_.size() == other.children_.size();
  }

  template <typename F> DefinedLang mapChildren(F &&fn) const {
    auto copy = *this;
    for (Id &id : copy.children_) {
      id = fn(id);
    }
    return copy;
  }

  friend bool operator==(const DefinedLang &lhs, const DefinedLang &rhs) {
    return lhs.op_ == rhs.op_ && lhs.children_ == rhs.children_;
  }

  friend bool operator!=(const DefinedLang &lhs, const DefinedLang &rhs) {
    return !(lhs == rhs);
  }

  friend bool operator<(const DefinedLang &lhs, const DefinedLang &rhs) {
    return std::tie(lhs.op_, lhs.children_) < std::tie(rhs.op_, rhs.children_);
  }

private:
  Symbol op_;
  llvm::SmallVector<Id, 2> children_;
};

template <typename Tag> struct LanguageOps<DefinedLang<Tag>> {
  static std::optional<DefinedLang<Tag>>
  fromOp(std::string_view op, const std::vector<Id> &children) {
    if (!Tag::allows(Symbol(op), children.size())) {
      return std::nullopt;
    }
    return DefinedLang<Tag>(Symbol(op), children);
  }

  static std::string display(const DefinedLang<Tag> &node) {
    return std::string(node.op().view());
  }
};

} // namespace lotus::egraph

#define LOTUS_EGRAPH_DEFINE_LANGUAGE(NAME, BODY)                               \
  struct NAME##Tag {                                                           \
    static bool allows(::lotus::egraph::Symbol op, size_t arity) {             \
      static const ::lotus::egraph::LanguageBuilder builder = [] {             \
        ::lotus::egraph::LanguageBuilder b;                                    \
        BODY return b;                                                         \
      }();                                                                     \
      return builder.allows(op, arity);                                        \
    }                                                                          \
  };                                                                           \
  using NAME = ::lotus::egraph::DefinedLang<NAME##Tag>

#define LOTUS_EGRAPH_LANG_OP(OP, ARITY)                                        \
  b.op(::lotus::egraph::Symbol(OP), ARITY);

template <> struct std::hash<lotus::egraph::DynamicLangDiscriminant> {
  size_t operator()(
      const lotus::egraph::DynamicLangDiscriminant &value) const noexcept {
    size_t seed = std::hash<lotus::egraph::Symbol>{}(value.op);
    lotus::egraph::hashCombine(seed, value.arity);
    return seed;
  }
};

template <> struct std::hash<lotus::egraph::DynamicLang> {
  size_t operator()(const lotus::egraph::DynamicLang &value) const noexcept {
    size_t seed = std::hash<lotus::egraph::Symbol>{}(value.op());
    for (lotus::egraph::Id child : value.children()) {
      lotus::egraph::hashCombine(seed, child);
    }
    return seed;
  }
};

template <typename Tag>
struct std::hash<lotus::egraph::DefinedLangDiscriminant<Tag>> {
  size_t operator()(
      const lotus::egraph::DefinedLangDiscriminant<Tag> &value) const noexcept {
    size_t seed = std::hash<lotus::egraph::Symbol>{}(value.op);
    lotus::egraph::hashCombine(seed, value.arity);
    return seed;
  }
};

template <typename Tag> struct std::hash<lotus::egraph::DefinedLang<Tag>> {
  size_t
  operator()(const lotus::egraph::DefinedLang<Tag> &value) const noexcept {
    size_t seed = std::hash<lotus::egraph::Symbol>{}(value.op());
    for (lotus::egraph::Id child : value.children()) {
      lotus::egraph::hashCombine(seed, child);
    }
    return seed;
  }
};
