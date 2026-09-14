// SPDX-License-Identifier: MIT
#include "CFL/Classical/Solvers/Engines/Common/Reachability.h"

#include <algorithm>
#include <limits>

namespace lotus::cfl::classical::common {
bool Reachability::coversSymbol(Symbol s) const {
  return std::binary_search(outputs_.begin(), outputs_.end(), s);
}
std::optional<Node> Reachability::representative(Node v) const {
  auto i = std::lower_bound(nodes_.begin(), nodes_.end(), v);
  if (i == nodes_.end() || *i != v)
    return std::nullopt;
  return reps_[static_cast<std::size_t>(i - nodes_.begin())];
}
bool Reachability::contains(Node u, Symbol s, Node v) const {
  if (!coversSymbol(s))
    throw std::invalid_argument("relation not requested / not guaranteed");
  if (terminal_.at(s))
    return std::binary_search(terminals_.begin(), terminals_.end(),
                              Fact{u, s, v});
  auto a = representative(u), b = representative(v);
  return a && b &&
         std::binary_search(facts_.begin(), facts_.end(), Fact{*a, s, *b});
}
std::size_t Reachability::expandedSize() const {
  std::size_t count = terminals_.size();
  const auto max = std::numeric_limits<std::size_t>::max();
  for (const auto &f : facts_) {
    const auto a = members_.at(f.source).size(),
               b = members_.at(f.target).size();
    if (a && b > (max - count) / a)
      throw ResourceLimit("expanded result size overflow");
    count += a * b;
  }
  return count;
}
std::size_t Reachability::expandedSize(Symbol symbol) const {
  if (!coversSymbol(symbol))
    throw std::invalid_argument("relation not requested / not guaranteed");
  std::size_t count = 0;
  const auto max = std::numeric_limits<std::size_t>::max();
  if (terminal_.at(symbol)) {
    auto fact = std::lower_bound(terminals_.begin(), terminals_.end(),
                                 Fact{0, symbol, 0});
    while (fact != terminals_.end() && fact->symbol == symbol) {
      ++count;
      ++fact;
    }
    return count;
  }
  auto fact =
      std::lower_bound(facts_.begin(), facts_.end(), Fact{0, symbol, 0});
  while (fact != facts_.end() && fact->symbol == symbol) {
    const auto sources = members_.at(fact->source).size();
    const auto targets = members_.at(fact->target).size();
    if (sources && targets > (max - count) / sources)
      throw ResourceLimit("expanded result size overflow");
    count += sources * targets;
    ++fact;
  }
  return count;
}
bool Reachability::visitFacts(
    Symbol symbol, const std::function<bool(const Fact &)> &visitor) const {
  if (!visitor)
    throw std::invalid_argument("null result visitor");
  if (!coversSymbol(symbol))
    throw std::invalid_argument("relation not requested / not guaranteed");
  if (terminal_.at(symbol)) {
    auto fact = std::lower_bound(terminals_.begin(), terminals_.end(),
                                 Fact{0, symbol, 0});
    while (fact != terminals_.end() && fact->symbol == symbol) {
      if (!visitor(*fact))
        return false;
      ++fact;
    }
    return true;
  }
  auto fact =
      std::lower_bound(facts_.begin(), facts_.end(), Fact{0, symbol, 0});
  while (fact != facts_.end() && fact->symbol == symbol) {
    for (Node source : members_.at(fact->source))
      for (Node target : members_.at(fact->target))
        if (!visitor({source, symbol, target}))
          return false;
    ++fact;
  }
  return true;
}
bool Reachability::visitSuccessors(
    Node source, Symbol symbol,
    const std::function<bool(Node)> &visitor) const {
  if (!visitor)
    throw std::invalid_argument("null successor visitor");
  if (!coversSymbol(symbol))
    throw std::invalid_argument("relation not requested / not guaranteed");
  if (terminal_.at(symbol)) {
    auto fact = std::lower_bound(terminals_.begin(), terminals_.end(),
                                 Fact{source, symbol, 0});
    while (fact != terminals_.end() && fact->symbol == symbol &&
           fact->source == source) {
      if (!visitor(fact->target))
        return false;
      ++fact;
    }
    return true;
  }
  const auto source_rep = representative(source);
  if (!source_rep)
    return true;
  auto fact = std::lower_bound(facts_.begin(), facts_.end(),
                               Fact{*source_rep, symbol, 0});
  while (fact != facts_.end() && fact->symbol == symbol &&
         fact->source == *source_rep) {
    for (Node target : members_.at(fact->target))
      if (!visitor(target))
        return false;
    ++fact;
  }
  return true;
}
bool Reachability::visitPredecessors(
    Symbol symbol, Node target,
    const std::function<bool(Node)> &visitor) const {
  if (!visitor)
    throw std::invalid_argument("null predecessor visitor");
  if (!coversSymbol(symbol))
    throw std::invalid_argument("relation not requested / not guaranteed");
  if (terminal_.at(symbol)) {
    auto fact = std::lower_bound(terminals_.begin(), terminals_.end(),
                                 Fact{0, symbol, 0});
    while (fact != terminals_.end() && fact->symbol == symbol) {
      if (fact->target == target && !visitor(fact->source))
        return false;
      ++fact;
    }
    return true;
  }
  const auto target_rep = representative(target);
  if (!target_rep)
    return true;
  auto fact =
      std::lower_bound(facts_.begin(), facts_.end(), Fact{0, symbol, 0});
  while (fact != facts_.end() && fact->symbol == symbol) {
    if (fact->target == *target_rep)
      for (Node source : members_.at(fact->source))
        if (!visitor(source))
          return false;
    ++fact;
  }
  return true;
}
void Reachability::forEachFact(const std::function<void(const Fact &)> &sink,
                               std::size_t limit) const {
  if (!sink)
    throw std::invalid_argument("null result sink");
  const auto n = expandedSize();
  if (limit && n > limit)
    throw ResourceLimit("result expansion limit exceeded");
  for (const auto &f : terminals_)
    sink(f);
  for (const auto &f : facts_)
    for (Node u : members_.at(f.source))
      for (Node v : members_.at(f.target))
        sink({u, f.symbol, v});
}
std::vector<Fact> Reachability::materialize(std::size_t limit) const {
  const auto n = expandedSize();
  if (limit && n > limit)
    throw ResourceLimit("result expansion limit exceeded");
  std::vector<Fact> out;
  out.reserve(n);
  forEachFact([&](const Fact &f) { out.push_back(f); }, limit);
  std::sort(out.begin(), out.end());
  return out;
}
Reachability
Reachability::build(std::vector<Node> nodes, std::vector<Node> reps,
                    std::vector<Symbol> outputs, std::vector<bool> terminal,
                    std::vector<Fact> facts, std::vector<Fact> terminals,
                    const Limits &limits) {
  if (nodes.size() != reps.size() ||
      !std::is_sorted(nodes.begin(), nodes.end()) ||
      std::adjacent_find(nodes.begin(), nodes.end()) != nodes.end())
    throw std::invalid_argument("invalid quotient node mapping");
  Reachability r;
  r.nodes_ = std::move(nodes);
  r.reps_ = std::move(reps);
  r.outputs_ = std::move(outputs);
  r.terminal_ = std::move(terminal);
  std::sort(r.outputs_.begin(), r.outputs_.end());
  r.outputs_.erase(std::unique(r.outputs_.begin(), r.outputs_.end()),
                   r.outputs_.end());
  for (Symbol s : r.outputs_)
    if (s >= r.terminal_.size())
      throw std::invalid_argument("bad output symbol");
  for (std::size_t i = 0; i < r.nodes_.size(); ++i) {
    auto p = std::lower_bound(r.nodes_.begin(), r.nodes_.end(), r.reps_[i]);
    if (p == r.nodes_.end() || *p != r.reps_[i] ||
        r.reps_[static_cast<std::size_t>(p - r.nodes_.begin())] != r.reps_[i])
      throw std::invalid_argument("non-canonical representative");
    r.members_[r.reps_[i]].push_back(r.nodes_[i]);
  }
  auto canonical = [](std::vector<Fact> &f) {
    std::sort(f.begin(), f.end());
    f.erase(std::unique(f.begin(), f.end()), f.end());
  };
  canonical(facts);
  canonical(terminals);
  for (const auto &f : facts)
    if (!r.coversSymbol(f.symbol) || r.terminal_.at(f.symbol) ||
        !r.members_.count(f.source) || !r.members_.count(f.target))
      throw std::invalid_argument("invalid quotient result edge");
  for (const auto &f : terminals)
    if (!r.coversSymbol(f.symbol) || !r.terminal_.at(f.symbol) ||
        !r.representative(f.source) || !r.representative(f.target))
      throw std::invalid_argument("invalid original terminal result edge");
  if (limits.max_result_entries &&
      (facts.size() > limits.max_result_entries ||
       terminals.size() > limits.max_result_entries - facts.size()))
    throw ResourceLimit("stored result limit exceeded");
  r.facts_ = std::move(facts);
  r.terminals_ = std::move(terminals);
  return r;
}
} // namespace lotus::cfl::classical::common
