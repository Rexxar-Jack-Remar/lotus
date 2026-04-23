#pragma once

#include "Solvers/EGraph/Pattern.h"

namespace lotus::egraph {

template <typename L, typename A>
using Condition = std::function<bool(EGraph<L, A> &, Id, const Subst &)>;

template <typename L, typename A = NoAnalysis<L>> class Rewrite {
public:
  Rewrite() = default;

  Rewrite(std::string name, Pattern<L> searcher, Pattern<L> applier,
          std::vector<Condition<L, A>> conditions = {})
      : name_(std::move(name)), searcher_(std::move(searcher)),
        applier_(std::move(applier)), conditions_(std::move(conditions)) {
    auto bound_vars = searcher_.vars();
    for (const auto &var : applier_.vars()) {
      if (std::find(bound_vars.begin(), bound_vars.end(), var) == bound_vars.end()) {
        throw std::runtime_error("Rewrite rhs refers to unbound variable");
      }
    }
  }

  const std::string &name() const { return name_; }

  std::vector<SearchMatches<L>> search(const EGraph<L, A> &egraph) const {
    return searcher_.search(egraph);
  }

  std::vector<Id> apply(EGraph<L, A> &egraph,
                        const std::vector<SearchMatches<L>> &matches) const {
    std::vector<Id> ids;
    for (const auto &match : matches) {
      for (const auto &subst : match.substs) {
        bool allowed = true;
        for (const auto &condition : conditions_) {
          if (!condition(egraph, match.eclass, subst)) {
            allowed = false;
            break;
          }
        }
        if (!allowed) {
          continue;
        }

        if (!egraph.analysis().allowEMatchingCycles() &&
            applier_.containsEClass(egraph, subst, match.eclass)) {
          continue;
        }

        Id rhs = applier_.apply(egraph, subst);
        Id merged = egraph.unite(match.eclass, rhs, name_);
        ids.push_back(merged);
      }
    }
    return ids;
  }

private:
  std::string name_;
  Pattern<L> searcher_;
  Pattern<L> applier_;
  std::vector<Condition<L, A>> conditions_;
};

template <typename L, typename A = NoAnalysis<L>>
inline Rewrite<L, A> makeRewrite(std::string name, std::string_view lhs,
                                 std::string_view rhs,
                                 std::vector<Condition<L, A>> conditions = {}) {
  return Rewrite<L, A>(std::move(name), Pattern<L>::parse(lhs), Pattern<L>::parse(rhs),
                       std::move(conditions));
}

} // namespace lotus::egraph
