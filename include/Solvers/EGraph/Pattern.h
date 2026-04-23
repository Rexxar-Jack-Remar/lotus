#pragma once

#include "Solvers/EGraph/EGraph.h"
#include "Solvers/EGraph/Subst.h"

#include <variant>

namespace lotus::egraph {

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
  explicit Pattern(PatternAst<L> ast) : ast_(std::move(ast)) {}

  static Pattern parse(std::string_view input) { return Pattern(PatternAst<L>::parse(input)); }

  const PatternAst<L> &ast() const { return ast_; }

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
    std::vector<SearchMatches<L>> matches;
    for (Id eclass : egraph.classIds()) {
      auto found = searchEClass(egraph, eclass);
      if (found && !found->substs.empty()) {
        matches.push_back(std::move(*found));
      }
    }
    return matches;
  }

  template <typename A>
  std::optional<SearchMatches<L>> searchEClass(const EGraph<L, A> &egraph,
                                               Id eclass) const {
    Subst subst;
    auto substs = matchPatternNode(egraph, ast_.root(), eclass, subst);

    if (substs.empty()) {
      return std::nullopt;
    }
    return SearchMatches<L>{eclass, std::move(substs)};
  }

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
  template <typename A>
  std::vector<Subst> matchPatternNode(const EGraph<L, A> &egraph, Id ast_id,
                                      Id eclass_id, const Subst &subst) const {
    const auto &item = ast_[ast_id];
    if (item.isVar()) {
      Subst next = subst;
      Id canonical = egraph.find(eclass_id);
      if (const Id *bound = next.get(item.var())) {
        if (egraph.find(*bound) != canonical) {
          return {};
        }
      } else {
        next.insert(item.var(), canonical);
      }
      return {next};
    }

    std::vector<Subst> out;
    const auto &klass = egraph[eclass_id];
    for (const auto &candidate : klass.nodes) {
      if (!candidate.matches(item.node())) {
        continue;
      }

      std::vector<Subst> frontier{subst};
      const auto &pattern_children = item.node().children();
      const auto &candidate_children = candidate.children();
      for (size_t i = 0; i < pattern_children.size(); ++i) {
        std::vector<Subst> next_frontier;
        for (const auto &current_subst : frontier) {
          auto matched =
              matchPatternNode(egraph, pattern_children[i], candidate_children[i],
                               current_subst);
          next_frontier.insert(next_frontier.end(), matched.begin(), matched.end());
        }
        frontier = std::move(next_frontier);
        if (frontier.empty()) {
          break;
        }
      }
      out.insert(out.end(), frontier.begin(), frontier.end());
    }
    return out;
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
};

template <typename L> inline Pattern<L> pattern(std::string_view text) {
  return Pattern<L>::parse(text);
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
