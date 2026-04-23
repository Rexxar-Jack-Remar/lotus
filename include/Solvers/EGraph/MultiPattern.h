#pragma once

#include "Solvers/EGraph/Pattern.h"

namespace lotus::egraph {

template <typename L> class MultiPattern {
public:
  MultiPattern() = default;
  explicit MultiPattern(std::vector<std::pair<Var, Pattern<L>>> clauses)
      : clauses_(std::move(clauses)) {}

  static MultiPattern parse(std::string_view input) {
    std::vector<std::pair<Var, Pattern<L>>> clauses;
    std::string text(input);
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find(',', start);
      std::string clause =
          trim(end == std::string::npos ? std::string_view(text).substr(start)
                                        : std::string_view(text).substr(start, end - start));
      if (!clause.empty()) {
        size_t eq = clause.find('=');
        if (eq == std::string::npos) {
          throw std::runtime_error("Malformed multipattern clause");
        }
        Var var = Var::parse(trim(std::string_view(clause).substr(0, eq)));
        std::string rhs = trim(std::string_view(clause).substr(eq + 1));
        if (rhs.empty()) {
          throw std::runtime_error("Malformed multipattern clause");
        }
        clauses.emplace_back(var, Pattern<L>::parse(rhs));
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return MultiPattern(std::move(clauses));
  }

  template <typename A>
  std::vector<Subst> search(const EGraph<L, A> &egraph) const {
    return searchClauses(egraph, 0, Subst{});
  }

  template <typename A>
  std::vector<Id> apply(EGraph<L, A> &egraph, const std::vector<Subst> &matches) const {
    std::vector<Id> added;
    for (const auto &match : matches) {
      Subst subst = match;
      for (size_t i = 0; i < clauses_.size(); ++i) {
        Id id = clauses_[i].second.apply(egraph, subst);
        if (const Id *existing = subst.get(clauses_[i].first)) {
          egraph.unite(id, *existing);
        } else {
          subst.insert(clauses_[i].first, id);
        }
        if (i == 0) {
          added.push_back(id);
        }
      }
    }
    return added;
  }

  std::vector<Var> vars() const {
    std::vector<Var> vars;
    for (const auto &[bound, pat] : clauses_) {
      vars.push_back(bound);
      for (const auto &var : pat.vars()) {
        vars.push_back(var);
      }
    }
    std::sort(vars.begin(), vars.end());
    vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
    return vars;
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
