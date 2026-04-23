#pragma once

#include "Solvers/EGraph/EGraph.h"
#include "Solvers/EGraph/Subst.h"

#include <variant>

namespace lotus::egraph {

template <typename L> class PatternProgram;

template <typename L> struct ENodeOrVar {
  std::variant<L, Var> value;

  ENodeOrVar() = default;
  explicit ENodeOrVar(const L &node) : value(node) {}
  explicit ENodeOrVar(const Var &var) : value(var) {}

  bool isVar() const { return std::holds_alternative<Var>(value); }
  bool isNode() const { return std::holds_alternative<L>(value); }

  const Var &var() const { return std::get<Var>(value); }
  const L &node() const { return std::get<L>(value); }
  L &node() { return std::get<L>(value); }

  const std::vector<Id> &children() const {
    static const std::vector<Id> empty;
    if (isVar()) {
      return empty;
    }
    return node().children();
  }

  std::vector<Id> &childrenMut() {
    if (isVar()) {
      throw std::runtime_error("Pattern variable has no mutable children");
    }
    return node().childrenMut();
  }

  bool matches(const ENodeOrVar<L> &other) const {
    if (isVar() || other.isVar()) {
      return false;
    }
    return node().matches(other.node());
  }

  Symbol discriminant() const {
    if (isVar()) {
      return var().name();
    }
    return displayNode(node());
  }

  template <typename F> ENodeOrVar mapChildren(F &&fn) const {
    if (isVar()) {
      return *this;
    }
    return ENodeOrVar(node().mapChildren(std::forward<F>(fn)));
  }

  friend bool operator==(const ENodeOrVar<L> &lhs, const ENodeOrVar<L> &rhs) {
    return lhs.value == rhs.value;
  }

  friend bool operator!=(const ENodeOrVar<L> &lhs, const ENodeOrVar<L> &rhs) {
    return !(lhs == rhs);
  }
};

template <typename L> struct LanguageOps<ENodeOrVar<L>> {
  static std::optional<ENodeOrVar<L>>
  fromOp(std::string_view op, const std::vector<Id> &children) {
    if (!op.empty() && op.front() == '?' && children.empty()) {
      return ENodeOrVar<L>(Var::parse(op));
    }
    auto node = LanguageOps<L>::fromOp(op, children);
    if (!node) {
      return std::nullopt;
    }
    return ENodeOrVar<L>(*node);
  }

  static std::string display(const ENodeOrVar<L> &item) {
    return item.isVar() ? item.var().name() : displayNode(item.node());
  }
};

template <typename L> using PatternAst = RecExpr<ENodeOrVar<L>>;

template <typename L> struct SearchMatches {
  Id eclass;
  std::vector<Subst> substs;
};

template <typename L> class Pattern {
public:
  Pattern() = default;
  explicit Pattern(PatternAst<L> ast);
  explicit Pattern(const RecExpr<L> &expr);

  static Pattern parse(std::string_view input) { return Pattern(PatternAst<L>::parse(input)); }

  const PatternAst<L> &ast() const { return ast_; }

  PatternAst<L> alphaRename() const {
    std::unordered_map<Var, Var> vars;
    PatternAst<L> renamed;

    auto mkvar = [](size_t i) {
      static constexpr const char *kNames[] = {"?x", "?y", "?z", "?w"};
      if (i < std::size(kNames)) {
        return Var::parse(kNames[i]);
      }
      return Var::parse("?v" + std::to_string(i - std::size(kNames)));
    };

    for (const auto &node : ast_.items()) {
      if (!node.isVar()) {
        renamed.add(node);
        continue;
      }
      auto [it, inserted] = vars.try_emplace(node.var(), mkvar(vars.size()));
      (void)inserted;
      renamed.add(ENodeOrVar<L>(it->second));
    }
    return renamed;
  }

  std::vector<Var> vars() const {
    std::vector<Var> vars;
    for (const auto &item : ast_.items()) {
      if (item.isVar()) {
        if (std::find(vars.begin(), vars.end(), item.var()) == vars.end()) {
          vars.push_back(item.var());
        }
      }
    }
    return vars;
  }

  template <typename A>
  std::vector<SearchMatches<L>> search(const EGraph<L, A> &egraph) const {
    return searchWithLimit(egraph, std::numeric_limits<size_t>::max());
  }

  template <typename A>
  std::vector<SearchMatches<L>> searchWithLimit(const EGraph<L, A> &egraph,
                                                size_t limit) const {
    std::vector<SearchMatches<L>> matches;
    if (limit == 0) {
      return matches;
    }

    std::vector<Id> candidates;
    if (ast_.empty()) {
      return matches;
    }
    const auto &root = ast_.items().back();
    if (root.isVar()) {
      candidates = egraph.classIds();
    } else {
      candidates = egraph.classesForOp(root.node().discriminant());
    }

    for (Id eclass : candidates) {
      if (limit == 0) {
        break;
      }
      auto found = searchEClassWithLimit(egraph, eclass, limit);
      if (found && !found->substs.empty()) {
        limit -= std::min(limit, found->substs.size());
        matches.push_back(std::move(*found));
      }
    }
    return matches;
  }

  template <typename A>
  std::optional<SearchMatches<L>> searchEClass(const EGraph<L, A> &egraph,
                                               Id eclass) const {
    return searchEClassWithLimit(egraph, eclass, std::numeric_limits<size_t>::max());
  }

  template <typename A>
  std::optional<SearchMatches<L>> searchEClassWithLimit(const EGraph<L, A> &egraph,
                                                        Id eclass, size_t limit) const;

  template <typename A>
  size_t nMatches(const EGraph<L, A> &egraph) const {
    size_t total = 0;
    for (const auto &match : search(egraph)) {
      total += match.substs.size();
    }
    return total;
  }

  template <typename A>
  Id apply(EGraph<L, A> &egraph, const Subst &subst) const {
    return applyNode(egraph, ast_.root(), subst);
  }

  template <typename A>
  bool containsEClass(const EGraph<L, A> &egraph, const Subst &subst,
                      Id target) const {
    return containsEClass(egraph, ast_.root(), subst, egraph.find(target));
  }

private:
  void compact() {
    std::unordered_map<std::string, Id> seen;
    std::vector<ENodeOrVar<L>> compacted;
    compacted.reserve(ast_.size());

    for (const auto &node : ast_.items()) {
      if (node.isVar()) {
        compacted.push_back(node);
        continue;
      }

      std::string key = displayNode(node.node()) + "#";
      for (Id child : node.node().children()) {
        key += std::to_string(child.value()) + ",";
      }

      auto it = seen.find(key);
      if (it == seen.end()) {
        seen.emplace(key, Id::fromIndex(compacted.size()));
      }
      compacted.push_back(node);
    }

    ast_ = PatternAst<L>(std::move(compacted));
  }

  template <typename A>
  Id applyNode(EGraph<L, A> &egraph, Id ast_id, const Subst &subst) const {
    const auto &item = ast_[ast_id];
    if (item.isVar()) {
      return subst.at(item.var());
    }

    const auto &node = item.node();
    auto materialized = node.mapChildren([&](Id child) { return applyNode(egraph, child, subst); });
    return egraph.add(materialized);
  }

  template <typename A>
  bool containsEClass(const EGraph<L, A> &egraph, Id ast_id, const Subst &subst,
                      Id target) const {
    const auto &item = ast_[ast_id];
    if (item.isVar()) {
      return egraph.find(subst.at(item.var())) == target;
    }

    for (Id child : item.node().children()) {
      if (containsEClass(egraph, child, subst, target)) {
        return true;
      }
    }
    return false;
  }

  PatternAst<L> ast_;
  std::shared_ptr<PatternProgram<L>> program_;
};

template <typename L> inline Pattern<L> pattern(std::string_view text) {
  return Pattern<L>::parse(text);
}

} // namespace lotus::egraph

#include "Solvers/EGraph/PatternMachine.h"

namespace lotus::egraph {

template <typename L> inline Pattern<L>::Pattern(PatternAst<L> ast) : ast_(std::move(ast)) {
  compact();
  program_ = std::make_shared<PatternProgram<L>>(PatternProgram<L>::compileFromPattern(*this));
}

template <typename L> inline Pattern<L>::Pattern(const RecExpr<L> &expr) {
  PatternAst<L> ast;
  for (const auto &node : expr.items()) {
    ast.add(ENodeOrVar<L>(node));
  }
  ast_ = std::move(ast);
  compact();
  program_ = std::make_shared<PatternProgram<L>>(PatternProgram<L>::compileFromPattern(*this));
}

template <typename L>
template <typename A>
inline std::optional<SearchMatches<L>>
Pattern<L>::searchEClassWithLimit(const EGraph<L, A> &egraph, Id eclass, size_t limit) const {
  if (limit == 0) {
    return std::nullopt;
  }
  auto substs = program_->runWithLimit(egraph, eclass, limit);
  if (substs.empty()) {
    return std::nullopt;
  }
  return SearchMatches<L>{eclass, std::move(substs)};
}

} // namespace lotus::egraph

template <typename L> struct std::hash<lotus::egraph::ENodeOrVar<L>> {
  size_t operator()(const lotus::egraph::ENodeOrVar<L> &value) const noexcept {
    size_t seed = 0;
    if (value.isVar()) {
      lotus::egraph::hashCombine(seed, value.var());
      return seed;
    }
    lotus::egraph::hashCombine(seed, value.node());
    return seed;
  }
};
