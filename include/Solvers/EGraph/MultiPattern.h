#pragma once

#include "Solvers/EGraph/Pattern.h"

namespace lotus::egraph {

template <typename L> class MultiPattern {
public:
  MultiPattern() = default;
  explicit MultiPattern(std::vector<std::pair<Var, Pattern<L>>> clauses)
      : clauses_(std::move(clauses)) {}

  template <typename A>
  std::vector<Subst> search(const EGraph<L, A> &egraph) const {
    return searchClauses(egraph, 0, Subst{});
  }

private:
  template <typename A>
  std::vector<Subst> searchClauses(const EGraph<L, A> &egraph, size_t index,
                                   const Subst &seed) const {
    if (index >= clauses_.size()) {
      return {seed};
    }

    std::vector<Subst> results;
    auto matches = clauses_[index].second.search(egraph);
    for (const auto &match : matches) {
      for (const auto &candidate : match.substs) {
        const Var &bound_var = clauses_[index].first;
        if (const Id *existing = seed.get(bound_var)) {
          if (egraph.find(*existing) != egraph.find(match.eclass)) {
            continue;
          }
        }

        if (!compatible(egraph, seed, candidate)) {
          continue;
        }

        Subst merged = merge(seed, candidate, bound_var, match.eclass);
        auto tail = searchClauses(egraph, index + 1, merged);
        results.insert(results.end(), tail.begin(), tail.end());
      }
    }
    return results;
  }

  template <typename A>
  static bool compatible(const EGraph<L, A> &egraph, const Subst &lhs,
                         const Subst &rhs) {
    for (const auto &[var, id] : lhs.bindings()) {
      if (const Id *other = rhs.get(var)) {
        if (egraph.find(*other) != egraph.find(id)) {
          return false;
        }
      }
    }
    return true;
  }

  static Subst merge(const Subst &lhs, const Subst &rhs, const Var &bound_var,
                     Id bound_id) {
    Subst merged = lhs;
    for (const auto &[var, id] : rhs.bindings()) {
      merged.insert(var, id);
    }
    merged.insert(bound_var, bound_id);
    return merged;
  }

  std::vector<std::pair<Var, Pattern<L>>> clauses_;
};

} // namespace lotus::egraph
