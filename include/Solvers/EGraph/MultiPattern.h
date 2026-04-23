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
          trim(end == std::string::npos
                   ? std::string_view(text).substr(start)
                   : std::string_view(text).substr(start, end - start));
      if (!clause.empty()) {
        std::vector<std::string> parts;
        size_t part_start = 0;
        while (part_start <= clause.size()) {
          size_t eq = clause.find('=', part_start);
          std::string part =
              trim(eq == std::string::npos
                       ? std::string_view(clause).substr(part_start)
                       : std::string_view(clause).substr(part_start,
                                                         eq - part_start));
          if (!part.empty()) {
            parts.push_back(std::move(part));
          }
          if (eq == std::string::npos) {
            break;
          }
          part_start = eq + 1;
        }

        if (parts.size() < 2) {
          throw std::runtime_error("Malformed multipattern clause");
        }

        Var var = Var::parse(parts.front());
        for (size_t i = 1; i < parts.size(); ++i) {
          clauses.emplace_back(var, Pattern<L>::parse(parts[i]));
        }
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
    std::vector<Subst> out;
    for (const auto &match :
         searchWithLimit(egraph, std::numeric_limits<size_t>::max())) {
      out.insert(out.end(), match.substs.begin(), match.substs.end());
    }
    return out;
  }

  template <typename A>
  std::vector<SearchMatches<L>> searchWithLimit(const EGraph<L, A> &egraph,
                                                size_t limit) const {
    if (clauses_.empty() || limit == 0) {
      return {};
    }

    std::vector<SearchMatches<L>> results;
    for (Id eclass : egraph.classIds()) {
      if (limit == 0) {
        break;
      }
      auto found = searchEClassWithLimit(egraph, eclass, limit);
      if (found && !found->substs.empty()) {
        limit -= std::min(limit, found->substs.size());
        results.push_back(std::move(*found));
      }
    }
    return results;
  }

  template <typename A>
  std::optional<SearchMatches<L>>
  searchEClassWithLimit(const EGraph<L, A> &egraph, Id eclass,
                        size_t limit) const {
    if (clauses_.empty() || limit == 0) {
      return std::nullopt;
    }
    if (clauses_.front().second.ast().size() == 1 &&
        clauses_.front().second.ast().items().front().isVar()) {
      throw std::runtime_error(
          "Bare pattern variable cannot be first in multipattern");
    }
    auto seed = Subst{};
    seed.insert(clauses_.front().first, eclass);
    auto results = searchClauses(egraph, 0, seed, &eclass, limit);
    if (results.empty()) {
      return std::nullopt;
    }
    return SearchMatches<L>{eclass, std::move(results)};
  }

  template <typename A> size_t nMatches(const EGraph<L, A> &egraph) const {
    size_t total = 0;
    for (const auto &match :
         searchWithLimit(egraph, std::numeric_limits<size_t>::max())) {
      total += match.substs.size();
    }
    return total;
  }

  template <typename A>
  std::vector<Id> apply(EGraph<L, A> &egraph,
                        const std::vector<Subst> &matches) const {
    if (egraph.areExplanationsEnabled()) {
      throw std::runtime_error(
          "Multipattern application does not support explanations");
    }
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

  std::vector<Var> applierVars() const {
    std::unordered_set<Var> bound;
    std::vector<Var> vars;
    for (const auto &[bound_var, pat] : clauses_) {
      for (const auto &var : pat.vars()) {
        if (!bound.count(var)) {
          vars.push_back(var);
        }
      }
      bound.insert(bound_var);
    }
    std::sort(vars.begin(), vars.end());
    vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
    return vars;
  }

  template <typename A>
  std::vector<Id>
  applyMatches(EGraph<L, A> &egraph,
               const std::vector<SearchMatches<L>> &matches) const {
    if (egraph.areExplanationsEnabled()) {
      throw std::runtime_error(
          "Multipattern application does not support explanations");
    }
    std::vector<Id> added;
    for (const auto &match : matches) {
      for (const auto &subst : match.substs) {
        Subst current = subst;
        for (size_t i = 0; i < clauses_.size(); ++i) {
          Id id = clauses_[i].second.apply(egraph, current);
          if (auto existing = current.insert(clauses_[i].first, id)) {
            egraph.unite(id, *existing);
          }
          if (i == 0) {
            added.push_back(id);
          }
        }
      }
    }
    return added;
  }

private:
  template <typename A>
  std::vector<Subst>
  searchClauses(const EGraph<L, A> &egraph, size_t index, const Subst &seed,
                const Id *root_eclass = nullptr,
                size_t limit = std::numeric_limits<size_t>::max()) const {
    if (index >= clauses_.size()) {
      return {seed};
    }
    if (limit == 0) {
      return {};
    }

    std::vector<Subst> results;
    std::vector<SearchMatches<L>> matches;
    if (index == 0 && root_eclass) {
      auto found = clauses_[index].second.searchEClassWithLimit(
          egraph, *root_eclass, limit);
      if (found) {
        matches.push_back(std::move(*found));
      }
    } else {
      matches = clauses_[index].second.searchWithLimit(egraph, limit);
    }
    for (const auto &match : matches) {
      for (const auto &candidate : match.substs) {
        if (results.size() >= limit) {
          return results;
        }
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
        auto tail = searchClauses(egraph, index + 1, merged, nullptr,
                                  limit - results.size());
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
