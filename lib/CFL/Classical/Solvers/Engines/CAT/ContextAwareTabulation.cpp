// SPDX-License-Identifier: MIT
#include "CFL/Classical/Solvers/Engines/CAT/ContextAwareTabulation.h"

#include "CFL/Classical/Solvers/Engines/Common/Tabulation.h"

#include <deque>
#include <set>

namespace lotus::cfl::classical::cat {
namespace d = common::detail;
namespace {
struct MutableSet {
  std::set<Symbol> terms;
  bool epsilon = false;
};
bool join(MutableSet &to, const MutableSet &from, bool include_epsilon = true) {
  bool change = false;
  for (Symbol s : from.terms)
    change |= to.terms.insert(s).second;
  if (include_epsilon && from.epsilon && !to.epsilon) {
    to.epsilon = true;
    change = true;
  }
  return change;
}
TerminalSet freeze(const MutableSet &s) {
  return {{s.terms.begin(), s.terms.end()}, s.epsilon};
}
void normalize(Grammar &g) { g.rules = d::RuleIndex(g).rules; }
} // namespace
bool TerminalSet::contains(Symbol t) const {
  return std::binary_search(terminals.begin(), terminals.end(), t);
}
GrammarAnalysis analyzeGrammar(const Grammar &g, FirstLastSemantics mode) {
  g.validate();
  d::RuleIndex index(g);
  const auto n = g.terminal.size();
  std::vector<MutableSet> first(n), last(n), left(n), right(n);
  for (std::size_t i = 0; i < n; ++i)
    if (g.terminal[i]) {
      first[i].terms.insert(static_cast<Symbol>(i));
      last[i].terms.insert(static_cast<Symbol>(i));
    }
  std::vector<std::vector<std::size_t>> depend(n);
  std::deque<std::size_t> work;
  std::vector<bool> pending(index.rules.size(), true);
  for (std::size_t i = 0; i < index.rules.size(); ++i) {
    work.push_back(i);
    const Rule &r = index.rules[i];
    if (r.arity >= 1)
      depend[r.first].push_back(i);
    if (r.arity == 2 && r.first != r.second)
      depend[r.second].push_back(i);
  }
  while (!work.empty()) {
    auto i = work.front();
    work.pop_front();
    pending[i] = false;
    const Rule r = index.rules[i];
    bool change = false;
    // Use temporaries so aliases such as A ::= A B never invalidate iteration.
    MutableSet f, l;
    if (r.arity == 0) {
      f.epsilon = true;
      l.epsilon = true;
    } else if (r.arity == 1) {
      f = first[r.first];
      l = last[r.first];
    } else {
      const bool literal = mode == FirstLastSemantics::PaperInferenceRules;
      join(f, first[r.first], literal);
      join(l, last[r.second], literal);
      if (first[r.first].epsilon)
        join(f, first[r.second], literal);
      if (last[r.second].epsilon)
        join(l, last[r.first], literal);
      if (!literal) {
        f.epsilon = first[r.first].epsilon && first[r.second].epsilon;
        l.epsilon = last[r.first].epsilon && last[r.second].epsilon;
      }
    }
    change |= join(first[r.lhs], f);
    change |= join(last[r.lhs], l);
    if (change)
      for (auto j : depend[r.lhs])
        if (!pending[j]) {
          pending[j] = true;
          work.push_back(j);
        }
  }
  GrammarAnalysis a;
  a.unary_uses.assign(n, false);
  for (const Rule &r : index.rules) {
    if (r.arity == 1)
      a.unary_uses[r.first] = true;
    else if (r.arity == 2) {
      join(left[r.second], last[r.first]);
      join(right[r.first], first[r.second]);
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    a.first.push_back(freeze(first[i]));
    a.last.push_back(freeze(last[i]));
    a.left.push_back(freeze(left[i]));
    a.right.push_back(freeze(right[i]));
  }
  return a;
}
bool UsageContexts::hasIncoming(Node v, Symbol s) const {
  if (s >= incoming.size())
    throw std::out_of_range("context symbol");
  return incoming_everywhere[s]
             ? std::binary_search(nodes.begin(), nodes.end(), v)
             : incoming[s].count(v) != 0;
}
bool UsageContexts::hasOutgoing(Node v, Symbol s) const {
  if (s >= outgoing.size())
    throw std::out_of_range("context symbol");
  return outgoing_everywhere[s]
             ? std::binary_search(nodes.begin(), nodes.end(), v)
             : outgoing[s].count(v) != 0;
}
UsageContexts annotateGraph(const Grammar &g, const Graph &graph,
                            const GrammarAnalysis &a) {
  d::Input input(g, graph, {});
  const auto n = g.terminal.size();
  if (a.left.size() != n || a.right.size() != n || a.first.size() != n ||
      a.last.size() != n || a.unary_uses.size() != n)
    throw std::invalid_argument("analysis/grammar size mismatch");
  UsageContexts c;
  c.nodes = input.nodes;
  c.incoming.resize(n);
  c.outgoing.resize(n);
  c.incoming_everywhere.resize(n);
  c.outgoing_everywhere.resize(n);
  std::vector<std::vector<Symbol>> left_users(n), right_users(n);
  for (std::size_t i = 0; i < n; ++i) {
    c.incoming_everywhere[i] = a.left[i].epsilon;
    c.outgoing_everywhere[i] = a.right[i].epsilon;
    for (Symbol t : a.left[i].terminals) {
      if (t >= n || !g.terminal[t])
        throw std::invalid_argument("nonterminal in Left set");
      if (!a.left[i].epsilon)
        left_users[t].push_back(static_cast<Symbol>(i));
    }
    for (Symbol t : a.right[i].terminals) {
      if (t >= n || !g.terminal[t])
        throw std::invalid_argument("nonterminal in Right set");
      if (!a.right[i].epsilon)
        right_users[t].push_back(static_cast<Symbol>(i));
    }
  }
  // Always scan ORIGINAL terminal edges, not the context-pruned graph.
  for (const auto &e : input.edges) {
    for (Symbol s : left_users[e.symbol])
      c.incoming[s].insert(input.nodes[e.target]);
    for (Symbol s : right_users[e.symbol])
      c.outgoing[s].insert(input.nodes[e.source]);
  }
  return c;
}
Transformation rewriteTransitivity(const Grammar &grammar) {
  grammar.validate();
  Transformation t;
  t.grammar = grammar;
  normalize(t.grammar);
  const std::size_t original_size = grammar.terminal.size();
  // The paper's last sentence in Sec.4.2.1 needs an applicability guard.
  // epsilon | B1 X | ... | Bk X denotes (B1 | ... | Bk)*, so reversing
  // ALL such recursions is safe. With another base it is not.
  for (std::size_t i = 0; i < original_size; ++i)
    if (!t.grammar.terminal[i]) {
      const Symbol x = static_cast<Symbol>(i);
      bool eps = false, prefix = false, safe = true;
      for (const Rule &r : t.grammar.rules)
        if (r.lhs == x) {
          if (r.arity == 0)
            eps = true;
          else if (r.arity == 1 && r.first == x) {
          } // no-op self unit
          else if (r.arity == 2 && r.second == x && r.first != x)
            prefix = true;
          else
            safe = false;
        }
      if (prefix && eps && safe) {
        for (Rule &r : t.grammar.rules)
          if (r.lhs == x && r.arity == 2)
            std::swap(r.first, r.second);
        t.events.push_back(
            {RewriteKind::EpsilonBasedClosureOrientation, x, std::nullopt});
      } else if (prefix)
        t.unsafe_orientation_changes_skipped.push_back(x);
    }
  for (std::size_t i = 0; i < original_size; ++i)
    if (!t.grammar.terminal[i]) {
      const Symbol a = static_cast<Symbol>(i);
      const Rule aa = Rule::binary(a, a, a);
      if (std::find(t.grammar.rules.begin(), t.grammar.rules.end(), aa) ==
          t.grammar.rules.end())
        continue;
      if (!d::nullable(t.grammar)[a]) {
        t.nonnullable_transitives_skipped.push_back(a);
        continue;
      }
      std::vector<Rule> binary_bases;
      std::vector<Symbol> cores;
      for (const Rule &r : t.grammar.rules)
        if (r.lhs == a) {
          if (r.arity == 1 && r.first != a)
            cores.push_back(r.first);
          if (r.arity == 2 && !(r.first == a && r.second == a))
            binary_bases.push_back(r);
        }
      std::optional<Symbol> core;
      if (!binary_bases.empty()) {
        if (t.grammar.terminal.size() >= std::numeric_limits<Symbol>::max())
          throw ResourceLimit("too many core symbols");
        core = static_cast<Symbol>(t.grammar.terminal.size());
        t.grammar.terminal.push_back(false);
        cores.push_back(*core);
      }
      std::vector<Rule> rules;
      for (const Rule &r : t.grammar.rules) {
        if (r.lhs == a)
          continue;
        if (r.arity == 2 && r.first == r.lhs && r.second == a) {
          for (Symbol c : cores)
            rules.push_back(Rule::binary(r.lhs, r.lhs, c));
        } else if (r.arity == 2 && r.first == a && r.second == r.lhs) {
          // Preserve order when a general orientation flip is not justified.
          for (Symbol c : cores)
            rules.push_back(Rule::binary(r.lhs, c, r.lhs));
        } else
          rules.push_back(r);
      }
      rules.push_back(Rule::epsilon(a));
      for (Symbol c : cores)
        rules.push_back(Rule::binary(a, a, c));
      if (core)
        for (const Rule &r : binary_bases)
          rules.push_back(Rule::binary(*core, r.first, r.second));
      t.grammar.rules = std::move(rules);
      normalize(t.grammar);
      t.events.push_back({RewriteKind::TransitiveCore, a, core});
    }
  normalize(t.grammar);
  t.grammar.validate();
  return t;
}
namespace {
class Kernel {
public:
  Kernel(const Grammar &g, const d::Input &input, const UsageContexts &contexts,
         const GrammarAnalysis &analysis, const Options &options)
      : g_(g), input_(input), ctx_(contexts), a_(analysis), o_(options), ri_(g),
        pn_(g.terminal.size(), false), dynamic_(g.terminal.size(), false) {
    std::vector<bool> trans(g.terminal.size(), false);
    for (std::size_t s = 0; s < g.terminal.size(); ++s) {
      const bool selected = s < input.selected.size() && input.selected[s];
      pn_[s] = !g.terminal[s] && !selected && o_.propagating_symbols;
      dynamic_[s] = !g.terminal[s] && o_.dynamic_skewing;
    }
    for (const Rule &r : ri_.rules)
      if (r.arity == 2) {
        if (!g.terminal[r.second] || r.lhs == r.first)
          pn_[r.first] = false;
        if (!g.terminal[r.first] || r.lhs == r.second)
          pn_[r.second] = false;
        if (r.lhs != r.second)
          dynamic_[r.first] = false;
        if (r.lhs != r.first)
          dynamic_[r.second] = false;
        if (r.lhs == r.first && r.lhs == r.second)
          trans[r.lhs] = true;
      }
    for (std::size_t s = 0; s < dynamic_.size(); ++s)
      dynamic_[s] = dynamic_[s] && trans[s];
    stats.nodes = input.nodes.size();
    stats.unique_base_edges = input.edges.size();
    for (std::size_t s = 0; s < g.terminal.size(); ++s) {
      stats.context_annotations +=
          contexts.incoming[s].size() + contexts.outgoing[s].size();
      stats.universal_contexts +=
          static_cast<std::size_t>(contexts.incoming_everywhere[s]) +
          static_cast<std::size_t>(contexts.outgoing_everywhere[s]);
    }
  }
  void run() {
    // Install every terminal before processing ANY propagating summary.
    for (const auto &e : input_.edges)
      derive(e, false);
    for (Symbol s : ri_.epsilon)
      for (std::size_t i = 0; i < input_.nodes.size(); ++i) {
        const auto v = static_cast<d::Vertex>(i);
        derive({v, s, v}, false);
      }
    while (!work_.empty()) {
      const Event event = work_.front();
      work_.pop_front();
      const auto e = event.edge;
      d::tick(stats.work_items, o_.limits.max_work_items,
              "CAT work-item limit");
      for (auto i : ri_.unary[e.symbol]) {
        ++stats.unary_applications;
        derive({e.source, ri_.rules[i].lhs, e.target}, false);
      }
      if (event.hasIn)
        for (auto i : ri_.second[e.symbol]) {
          const Rule r = ri_.rules[i];
          const d::Row *row = relation_.in(r.first, e.source);
          if (!row)
            continue;
          const auto end = row->list.size();
          for (std::size_t j = 0; j < end; ++j) {
            const auto u = row->list[j];
            ++stats.binary_join_pairs;
            derive({u, r.lhs, e.target}, r.lhs == r.first && r.lhs == r.second);
          }
        }
      if (event.hasOut)
        for (auto i : ri_.first[e.symbol]) {
          const Rule r = ri_.rules[i];
          const d::Row *row = relation_.out(r.second, e.target);
          if (!row)
            continue;
          const auto end = row->list.size();
          for (std::size_t j = 0; j < end; ++j) {
            const auto v = row->list[j];
            ++stats.binary_join_pairs;
            derive({e.source, r.lhs, v}, r.lhs == r.first && r.lhs == r.second);
          }
        }
    }
    stats.graph_degree = relation_.degree();
  }
  Stats stats;
  std::unordered_set<d::Edge, d::EdgeHash> output;

private:
  struct Event {
    d::Edge edge;
    bool hasIn, hasOut;
  };
  void derive(const d::Edge &e, bool transitive) {
    ++stats.attempts;
    if (e.symbol < input_.selected.size() && input_.selected[e.symbol] &&
        !g_.terminal[e.symbol]) {
      if (!output.count(e)) {
        if (o_.limits.max_result_entries &&
            output.size() >= o_.limits.max_result_entries)
          throw ResourceLimit("CAT stored result limit");
        output.insert(e);
      }
    }
    const bool hasIn = ctx_.incoming_everywhere[e.symbol] ||
                       ctx_.incoming[e.symbol].count(input_.nodes[e.source]);
    const bool hasOut = ctx_.outgoing_everywhere[e.symbol] ||
                        ctx_.outgoing[e.symbol].count(input_.nodes[e.target]);
    if (!(hasIn || hasOut || a_.unary_uses[e.symbol])) {
      ++stats.fully_pruned_attempts;
      return;
    }
    const bool staticPE = pn_[e.symbol];
    const bool dynamicPE = !staticPE && transitive && dynamic_[e.symbol];
    const bool unindexed = staticPE || dynamicPE || !(hasIn || hasOut);
    if (relation_.contains(e)) {
      ++stats.duplicate_attempts;
      return;
    }
    if (unindexed) {
      // Algorithm-2 lines 24-27 omit a seen set for unary-only facts. Such
      // facts occupy neither adjacency and MUST be memoized to terminate unit
      // cycles.
      if (unindexed_.count(e)) {
        ++stats.duplicate_attempts;
        return;
      }
      d::tick(stats.successful_insertions, o_.limits.max_insertions,
              "CAT insertion limit");
      unindexed_.insert(e);
      ++stats.unindexed_insertions;
      if (staticPE)
        ++stats.propagating_insertions;
      if (dynamicPE)
        ++stats.dynamic_insertions;
    } else {
      d::tick(stats.successful_insertions, o_.limits.max_insertions,
              "CAT insertion limit");
      if (unindexed_.count(e))
        ++stats.promotions;
      relation_.insert(e, hasIn, hasOut);
      if (hasIn && hasOut)
        ++stats.fully_indexed_insertions;
      else if (hasIn)
        ++stats.outgoing_only_insertions;
      else
        ++stats.incoming_only_insertions;
    }
    work_.push_back({e, hasIn, hasOut});
    stats.peak_worklist = std::max(stats.peak_worklist, work_.size());
  }
  const Grammar &g_;
  const d::Input &input_;
  const UsageContexts &ctx_;
  const GrammarAnalysis &a_;
  const Options &o_;
  d::RuleIndex ri_;
  std::vector<bool> pn_, dynamic_;
  d::Relation relation_;
  std::unordered_set<d::Edge, d::EdgeHash> unindexed_;
  std::deque<Event> work_;
};
} // namespace
Result solve(const Grammar &grammar, const Graph &graph,
             const Options &options) {
  const d::Input input(grammar, graph, options.query);
  Result result;
  Grammar working = grammar;
  if (options.conservative_static_skewing) {
    auto t = skewed::skewConservatively(working, input.outputs);
    working = std::move(t.grammar);
    result.static_skewing_rewrites = std::move(t.rewrites);
  }
  if (options.rewrite_transitivity)
    result.transformation = rewriteTransitivity(working);
  else {
    result.transformation.grammar = working;
    normalize(result.transformation.grammar);
  }
  result.working_grammar = result.transformation.grammar;
  result.analysis = analyzeGrammar(result.working_grammar, options.first_last);
  const auto ctx =
      annotateGraph(result.working_grammar, graph, result.analysis);
  Kernel kernel(result.working_grammar, input, ctx, result.analysis, options);
  kernel.run();
  std::vector<Fact> facts, terminals;
  for (const auto &e : kernel.output)
    facts.push_back({input.nodes[e.source], e.symbol, input.nodes[e.target]});
  for (const auto &e : input.edges)
    if (input.selected[e.symbol])
      terminals.push_back(
          {input.nodes[e.source], e.symbol, input.nodes[e.target]});
  result.reachability = Reachability::build(
      input.nodes, input.nodes, input.outputs, grammar.terminal,
      std::move(facts), std::move(terminals), options.limits);
  result.stats = kernel.stats;
  return result;
}
} // namespace lotus::cfl::classical::cat
