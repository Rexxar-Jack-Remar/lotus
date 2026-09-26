// SPDX-License-Identifier: MIT
#include "CFL/Classical/Solvers/Engines/IEOCE/IterativeEpoch.h"

#include "CFL/Classical/Solvers/Engines/Common/Tabulation.h"

#include <functional>
#include <set>

namespace lotus::cfl::classical::ieoce {
namespace d = common::detail;
namespace {
void validateReverseGrammar(const Grammar &g,
                            const std::vector<Symbol> &reverse) {
  if (reverse.size() != g.terminal.size())
    throw std::invalid_argument(
        "bidirected mode requires a total reverse mapping");
  for (std::size_t i = 0; i < reverse.size(); ++i) {
    const Symbol s = reverse[i];
    if (s >= reverse.size() || reverse[s] != i ||
        g.terminal[i] != g.terminal[s])
      throw std::invalid_argument(
          "reverse mapping must be an involution preserving terminal status");
  }
  const auto rules = d::RuleIndex(g).rules;
  for (const Rule &r : rules) {
    Rule mirror = Rule::epsilon(reverse[r.lhs]);
    if (r.arity == 1)
      mirror = Rule::unary(reverse[r.lhs], reverse[r.first]);
    else if (r.arity == 2)
      mirror =
          Rule::binary(reverse[r.lhs], reverse[r.second], reverse[r.first]);
    if (!std::binary_search(rules.begin(), rules.end(), mirror))
      throw std::invalid_argument(
          "bidirected grammar is not syntactically reverse closed");
  }
}
TransitivityCheck check(const Grammar &g, Symbol a,
                        const std::vector<Symbol> &outputs, Direction direction,
                        const std::vector<Symbol> &reverse) {
  if (a >= g.terminal.size())
    throw std::invalid_argument("undeclared transitive symbol");
  const auto rules = d::RuleIndex(g).rules;
  const auto has = [&](Rule r) {
    return std::binary_search(rules.begin(), rules.end(), r);
  };
  const auto left = [&](Symbol x) {
    return has(Rule::binary(x, a, x)) || (direction == Direction::Bidirected &&
                                          has(Rule::binary(x, reverse[a], x)));
  };
  const auto right = [&](Symbol x) {
    return has(Rule::binary(x, x, a)) || (direction == Direction::Bidirected &&
                                          has(Rule::binary(x, x, reverse[a])));
  };
  TransitivityCheck c;
  c.symbol = a;
  c.doubly_recursive = has(Rule::binary(a, a, a));
  for (Symbol s : outputs)
    if (!g.terminal[s] && !(left(s) && right(s)))
      c.nonabsorbing_targets.push_back(s);
  for (const Rule &r : rules)
    if (r.arity == 2 && !(right(r.first) || left(r.second)))
      c.intransitive_combinations.push_back(r);
  c.eligible =
      c.nonabsorbing_targets.empty() && c.intransitive_combinations.empty();
  return c;
}
// Sparse minimum-equivalent-graph storage. The full A closure is kept in the
// main relation, as in the paper, not duplicated in a second reachability
// table.
class MEG {
public:
  std::vector<d::Vertex> successors(d::Vertex u) const {
    return values(out_, u);
  }
  std::vector<d::Vertex> predecessors(d::Vertex u) const {
    return values(in_, u);
  }
  void add(d::Vertex u, d::Vertex v) {
    if (u == v)
      return; // reflexive A facts remain in the main graph
    if (out_[u].insert(v).second) {
      in_[v].insert(u);
      ++size_;
    }
  }
  bool erase(d::Vertex u, d::Vertex v) {
    auto it = out_.find(u);
    if (it == out_.end() || !it->second.erase(v))
      return false;
    in_.at(v).erase(u);
    --size_;
    return true;
  }
  std::vector<std::pair<d::Vertex, d::Vertex>> edges() const {
    std::vector<std::pair<d::Vertex, d::Vertex>> es;
    es.reserve(size_);
    for (const auto &p : out_)
      for (auto v : p.second)
        es.emplace_back(p.first, v);
    std::sort(es.begin(), es.end());
    return es;
  }
  std::size_t size() const { return size_; }

private:
  using Rows = std::unordered_map<d::Vertex, std::set<d::Vertex>>;
  static std::vector<d::Vertex> values(const Rows &r, d::Vertex v) {
    auto it = r.find(v);
    if (it == r.end())
      return {};
    return {it->second.begin(), it->second.end()};
  }
  Rows out_, in_;
  std::size_t size_ = 0;
};

class Kernel {
public:
  Kernel(const Grammar &g, const d::Input &input, const Options &options,
         std::optional<Symbol> a, bool ordered)
      : g_(g), input_(input), o_(options), a_(a), ordered_(ordered), ri_(g),
        generating_(ri_.rules.size(), false),
        generating_use_(g.terminal.size(), false),
        other_use_(g.terminal.size(), false), parent_(input.nodes.size()) {
    std::iota(parent_.begin(), parent_.end(), d::Vertex{0});
    stats.nodes = input.nodes.size();
    stats.quotient_nodes = stats.nodes;
    stats.unique_base_edges = input.edges.size();
    stats.ordinary_fallback = !a;
    stats.ordered_enabled = ordered;
    for (std::size_t i = 0; i < ri_.rules.size(); ++i) {
      const Rule &r = ri_.rules[i];
      generating_[i] =
          a && r.lhs == *a &&
          (r.arity == 1 || (r.arity == 2 && (r.first != *a || r.second != *a)));
      auto &use = generating_[i] ? generating_use_ : other_use_;
      if (r.arity >= 1)
        use[r.first] = true;
      if (r.arity == 2)
        use[r.second] = true;
    }
  }
  void run() {
    for (const auto &e : input_.edges)
      seed(e);
    for (Symbol s : ri_.epsilon)
      for (std::size_t i = 0; i < input_.nodes.size(); ++i) {
        const auto v = static_cast<d::Vertex>(i);
        seed({v, s, v});
      }
    if (!a_) {
      while (!main_.empty()) {
        auto e = main_.pop();
        pop(false);
        process(e, Phase::All);
      }
    } else
      while (!main_.empty() || !other_.empty()) {
        d::tick(stats.epochs, o_.limits.max_epochs, "IEOCE epoch limit");
        EpochStats trace;
        trace.epoch = stats.epochs;
        const auto old_gen = stats.generating_work,
                   old_other = stats.other_work,
                   old_merged = stats.merged_nodes;
        // Algorithm 2, GenerateTransitiveEdges. Newly obtained A facts are also
        // replayed here when A itself occurs in an A-generating production.
        // This closes the general-grammar scheduling hole noted in
        // PAPER_NOTES.md.
        while (!main_.empty()) {
          auto e = main_.pop();
          pop(true);
          process(e, Phase::Generating);
          if (other_use_[e.symbol])
            queueOther(e);
        }
        if (o_.check_invariants)
          verify(false);
        collapse();
        if (o_.check_invariants)
          verify(true);
        while (!other_.empty()) {
          auto e = other_.pop();
          pop(false);
          process(e, Phase::Other);
        }
        trace.generating_work = stats.generating_work - old_gen;
        trace.other_work = stats.other_work - old_other;
        trace.nodes_merged = stats.merged_nodes - old_merged;
        trace.remaining_nodes = stats.quotient_nodes;
        trace.graph_facts = facts_.size();
        trace.meg_edges = meg_.size();
        if (o_.trace_epochs)
          traces.push_back(trace);
      }
    if (o_.check_invariants)
      verify(true);
    stats.graph_facts = facts_.size();
    stats.meg_edges = meg_.size();
  }
  Reachability result() {
    std::vector<Node> reps;
    reps.reserve(parent_.size());
    for (std::size_t i = 0; i < parent_.size(); ++i)
      reps.push_back(input_.nodes[find(static_cast<d::Vertex>(i))]);
    std::vector<Fact> facts, terminals;
    for (const auto &e : facts_)
      if (input_.selected[e.symbol] && !g_.terminal[e.symbol])
        facts.push_back(
            {input_.nodes[e.source], e.symbol, input_.nodes[e.target]});
    for (const auto &e : input_.edges)
      if (input_.selected[e.symbol])
        terminals.push_back(
            {input_.nodes[e.source], e.symbol, input_.nodes[e.target]});
    return Reachability::build(input_.nodes, std::move(reps), input_.outputs,
                               g_.terminal, std::move(facts),
                               std::move(terminals), o_.limits);
  }
  Stats stats;
  std::vector<EpochStats> traces;

private:
  enum class Phase { All, Generating, Other };
  d::Vertex find(d::Vertex v) {
    d::Vertex r = v;
    while (parent_[r] != r)
      r = parent_[r];
    while (parent_[v] != v) {
      const auto next = parent_[v];
      parent_[v] = r;
      v = next;
    }
    return r;
  }
  void peak() {
    stats.peak_worklist =
        std::max(stats.peak_worklist, main_.queue.size() + other_.queue.size());
  }
  void queueMain(d::Edge e) {
    main_.push(e);
    peak();
  }
  void queueOther(d::Edge e) {
    other_.push(e);
    peak();
  }
  void pop(bool generating) {
    d::tick(stats.work_items, o_.limits.max_work_items,
            "IEOCE work-item limit");
    if (generating)
      ++stats.generating_work;
    else
      ++stats.other_work;
  }
  bool rawInsert(d::Edge e) {
    ++stats.attempts;
    if (relation_.contains(e)) {
      ++stats.duplicate_attempts;
      return false;
    }
    d::tick(stats.successful_insertions, o_.limits.max_insertions,
            "IEOCE insertion limit");
    relation_.insert(e);
    facts_.push_back(e);
    return true;
  }
  void scheduleA(d::Edge e) {
    if (other_use_[e.symbol])
      queueOther(e);
    if (generating_use_[e.symbol])
      queueMain(e);
  }
  void seed(d::Edge e) {
    if (ordered_ && e.symbol == *a_)
      insertMEG(e.source, e.target);
    else if (rawInsert(e))
      queueMain(e);
  }
  bool reachA(d::Vertex u, d::Vertex v) const {
    return relation_.contains({u, *a_, v});
  }
  // Algorithms 4 and 5: insert a primary A edge, propagate the newly connected
  // predecessor/successor pairs, prune redundant MEG edges for non-back edges.
  // isaBackEdge is evaluated BEFORE TravBackward (printed Algorithm 4 uses it
  // before assigning it). All traversals use explicit stacks, not C++
  // recursion.
  void insertMEG(d::Vertex u, d::Vertex v) {
    if (reachA(u, v)) {
      ++stats.attempts;
      ++stats.duplicate_attempts;
      return;
    }
    ++stats.meg_insertions;
    const bool is_back = (u == v || reachA(v, u));
    std::vector<d::Vertex> backward{u};
    std::unordered_set<d::Vertex> visited;
    while (!backward.empty()) {
      const auto p = backward.back();
      backward.pop_back();
      if (!visited.insert(p).second)
        continue;
      const auto pred = meg_.predecessors(p);
      if (!is_back)
        for (auto k : meg_.successors(p))
          if (k == v || reachA(v, k))
            if (meg_.erase(p, k))
              ++stats.meg_edges_removed;
      std::vector<d::Vertex> forward{v};
      while (!forward.empty()) {
        const auto q = forward.back();
        forward.pop_back();
        if (reachA(p, q))
          continue;
        const d::Edge e{p, *a_, q};
        if (rawInsert(e)) {
          ++stats.transitive_updates;
          scheduleA(e);
        }
        for (auto k : meg_.successors(q))
          if (!reachA(p, k))
            forward.push_back(k);
      }
      // Even an already connected predecessor can have a now-redundant
      // direct MEG edge. Visit it for reduction; forward propagation still
      // stops immediately when its A pair already exists.
      for (auto k : pred)
        backward.push_back(k);
    }
    meg_.add(u, v);
  }
  void derive(d::Edge e, Phase phase) {
    if (phase == Phase::Generating) {
      if (ordered_)
        insertMEG(e.source, e.target);
      else if (rawInsert(e))
        scheduleA(e);
    } else if (rawInsert(e))
      queueMain(e);
  }
  bool accepts(std::size_t i, Phase p) const {
    if (p == Phase::All)
      return true;
    if (p == Phase::Generating)
      return generating_[i];
    const Rule &r = ri_.rules[i];
    // Both occurrences of A ::= A A are handled by InsertMEGEdge in Iea-Ocr.
    return !generating_[i] && !(ordered_ && r.arity == 2 && r.lhs == *a_ &&
                                r.first == *a_ && r.second == *a_);
  }
  // Algorithm 3: stop a branch when its summary edge already exists. Events
  // for newly added A reachability still execute ordinary complementary joins.
  void orderedForward(d::Edge e, Symbol lhs) {
    std::vector<d::Vertex> work = meg_.successors(e.target);
    while (!work.empty()) {
      auto v = work.back();
      work.pop_back();
      ++stats.ordered_steps;
      const d::Edge next{e.source, lhs, v};
      if (relation_.contains(next)) {
        ++stats.ordered_prunes;
        continue;
      }
      if (rawInsert(next))
        queueMain(next);
      for (auto w : meg_.successors(v))
        work.push_back(w);
    }
  }
  void orderedBackward(d::Edge e, Symbol lhs) {
    std::vector<d::Vertex> work = meg_.predecessors(e.source);
    while (!work.empty()) {
      auto u = work.back();
      work.pop_back();
      ++stats.ordered_steps;
      const d::Edge next{u, lhs, e.target};
      if (relation_.contains(next)) {
        ++stats.ordered_prunes;
        continue;
      }
      if (rawInsert(next))
        queueMain(next);
      for (auto w : meg_.predecessors(u))
        work.push_back(w);
    }
  }
  void process(d::Edge e, Phase phase) {
    for (auto i : ri_.unary[e.symbol])
      if (accepts(i, phase)) {
        ++stats.unary_applications;
        derive({e.source, ri_.rules[i].lhs, e.target}, phase);
      }
    for (auto i : ri_.first[e.symbol])
      if (accepts(i, phase)) {
        const Rule r = ri_.rules[i];
        if (ordered_ && phase == Phase::Other && r.second == *a_ &&
            r.lhs == e.symbol) {
          orderedForward(e, r.lhs);
          continue;
        }
        const d::Row *row = relation_.out(r.second, e.target);
        if (!row)
          continue;
        const auto end = row->list.size();
        for (std::size_t j = 0; j < end; ++j) {
          const auto v = row->list[j];
          ++stats.binary_join_pairs;
          derive({e.source, r.lhs, v}, phase);
        }
      }
    for (auto i : ri_.second[e.symbol])
      if (accepts(i, phase)) {
        const Rule r = ri_.rules[i];
        if (ordered_ && phase == Phase::Other && r.first == *a_ &&
            r.lhs == e.symbol) {
          orderedBackward(e, r.lhs);
          continue;
        }
        const d::Row *row = relation_.in(r.first, e.source);
        if (!row)
          continue;
        const auto end = row->list.size();
        for (std::size_t j = 0; j < end; ++j) {
          const auto u = row->list[j];
          ++stats.binary_join_pairs;
          derive({u, r.lhs, e.target}, phase);
        }
      }
  }
  // Iterative Tarjan SCC. The SCC primitive is interchangeable with the
  // Nuutila implementation named in the paper; complexity remains O(V+E).
  std::vector<std::vector<d::Vertex>> components() {
    const auto n = parent_.size();
    std::vector<std::vector<d::Vertex>> adj(n);
    if (ordered_) {
      for (auto e : meg_.edges())
        adj[e.first].push_back(e.second);
    } else {
      for (const auto &e : facts_)
        if (e.symbol == *a_)
          adj[e.source].push_back(e.target);
    }
    std::vector<std::int64_t> index(n, -1);
    std::vector<std::size_t> low(n, 0);
    std::vector<bool> on_stack(n, false);
    std::vector<d::Vertex> stack;
    std::size_t clock = 0;
    struct Frame {
      d::Vertex v;
      std::size_t next;
    };
    std::vector<Frame> dfs;
    std::vector<std::vector<d::Vertex>> result;
    auto enter = [&](d::Vertex v) {
      index[v] = static_cast<std::int64_t>(clock);
      low[v] = clock++;
      on_stack[v] = true;
      stack.push_back(v);
      dfs.push_back({v, 0});
    };
    for (std::size_t r = 0; r < n; ++r) {
      const auto root = static_cast<d::Vertex>(r);
      if (parent_[root] != root || index[root] != -1)
        continue;
      enter(root);
      while (!dfs.empty()) {
        const d::Vertex v = dfs.back().v;
        if (dfs.back().next < adj[v].size()) {
          const auto w = adj[v][dfs.back().next++];
          if (index[w] == -1) {
            enter(w);
            continue;
          }
          if (on_stack[w])
            low[v] = std::min(low[v], static_cast<std::size_t>(index[w]));
        } else {
          if (low[v] == static_cast<std::size_t>(index[v])) {
            std::vector<d::Vertex> c;
            while (true) {
              const auto w = stack.back();
              stack.pop_back();
              on_stack[w] = false;
              c.push_back(w);
              if (w == v)
                break;
            }
            if (c.size() > 1)
              result.push_back(std::move(c));
          }
          dfs.pop_back();
          if (!dfs.empty()) {
            const auto p = dfs.back().v;
            low[p] = std::min(low[p], low[v]);
          }
        }
      }
    }
    return result;
  }
  void verify(bool reduced) const {
    for (const auto &e : facts_) {
      if (parent_[e.source] != e.source || parent_[e.target] != e.target ||
          !relation_.contains(e))
        throw std::logic_error("non-canonical quotient relation");
    }
    for (const auto *w : {&main_, &other_})
      for (const auto &e : w->queue)
        if (parent_[e.source] != e.source || parent_[e.target] != e.target ||
            !relation_.contains(e))
          throw std::logic_error("non-canonical quotient worklist");
    if (!ordered_)
      return;
    for (std::size_t i = 0; i < parent_.size(); ++i) {
      const auto u = static_cast<d::Vertex>(i);
      if (parent_[u] != u)
        continue;
      std::vector<bool> reached(parent_.size(), false);
      auto work = meg_.successors(u);
      while (!work.empty()) {
        const auto v = work.back();
        work.pop_back();
        if (reached[v])
          continue;
        reached[v] = true;
        for (auto w : meg_.successors(v))
          work.push_back(w);
      }
      for (std::size_t j = 0; j < parent_.size(); ++j) {
        const auto v = static_cast<d::Vertex>(j);
        if (parent_[v] != v)
          continue;
        if ((u != v && reached[v] != reachA(u, v)) ||
            (reached[v] && !reachA(u, v)))
          throw std::logic_error(
              "MEG reachability differs from main A closure");
      }
      if (reduced)
        for (auto v : meg_.successors(u))
          for (auto k : meg_.successors(u))
            if (k != v && reachA(k, v))
              throw std::logic_error("post-collapse MEG has a redundant edge");
    }
  }
  void collapse() {
    ++stats.scc_passes;
    const auto comps = components();
    if (comps.empty())
      return;
    for (const auto &c : comps) {
      const auto representative = *std::min_element(c.begin(), c.end());
      for (auto v : c)
        parent_[v] = representative;
      ++stats.collapsed_components;
      stats.merged_nodes += c.size() - 1;
      stats.quotient_nodes -= c.size() - 1;
    }
    for (std::size_t i = 0; i < parent_.size(); ++i)
      (void)find(static_cast<d::Vertex>(i));
    d::Relation projected;
    std::vector<d::Edge> facts;
    for (auto e : facts_) {
      e.source = find(e.source);
      e.target = find(e.target);
      if (projected.insert(e))
        facts.push_back(e);
    }
    relation_ = std::move(projected);
    facts_ = std::move(facts);
    if (ordered_) {
      MEG projected_meg;
      for (auto p : meg_.edges())
        projected_meg.add(find(p.first), find(p.second));
      meg_ = std::move(projected_meg);
      // Contraction removes every cycle. Reduce the resulting DAG: u->v is
      // redundant iff another direct successor k can reach v. A closure is
      // already available in the main graph. This also cleans up edges whose
      // deletion was intentionally deferred for back-edge insertions.
      const auto old_edges = meg_.edges();
      for (auto p : old_edges) {
        for (auto k : meg_.successors(p.first))
          if (k != p.second && reachA(k, p.second)) {
            if (meg_.erase(p.first, p.second))
              ++stats.meg_edges_removed;
            break;
          }
      }
    }
    // Reindex ALL labels and worklists. Quotienting changes endpoints of
    // already processed facts; those facts can participate in new joins.
    // Queue replay is finite: at most |V|-1 nodes can ever be merged.
    main_.clear();
    other_.clear();
    for (const auto &e : facts_) {
      if (generating_use_[e.symbol]) {
        queueMain(e);
        ++stats.quotient_replays;
      }
      if (other_use_[e.symbol]) {
        queueOther(e);
        ++stats.quotient_replays;
      }
    }
  }
  const Grammar &g_;
  const d::Input &input_;
  const Options &o_;
  std::optional<Symbol> a_;
  bool ordered_;
  d::RuleIndex ri_;
  std::vector<bool> generating_, generating_use_, other_use_;
  std::vector<d::Vertex> parent_;
  d::Relation relation_;
  std::vector<d::Edge> facts_;
  MEG meg_;
  d::Worklist main_, other_;
};
} // namespace
TransitivityCheck checkTransitiveSymbol(const Grammar &g, Symbol a,
                                        const Query &q, Direction dir,
                                        const std::vector<Symbol> &reverse) {
  d::Input input(g, {}, q);
  if (dir == Direction::Bidirected)
    validateReverseGrammar(g, reverse);
  return check(g, a, input.outputs, dir, reverse);
}
std::vector<Symbol> findTransitiveSymbols(const Grammar &g, const Query &q,
                                          Direction dir,
                                          const std::vector<Symbol> &reverse) {
  d::Input input(g, {}, q);
  if (dir == Direction::Bidirected)
    validateReverseGrammar(g, reverse);
  std::vector<Symbol> result;
  for (std::size_t i = 0; i < g.terminal.size(); ++i)
    if (check(g, static_cast<Symbol>(i), input.outputs, dir, reverse).eligible)
      result.push_back(static_cast<Symbol>(i));
  return result;
}
Result solve(const Grammar &grammar, const Graph &graph,
             const Options &options) {
  d::Input input(grammar, graph, options.query);
  if (options.direction == Direction::Bidirected) {
    validateReverseGrammar(grammar, options.reverse_symbols);
    const std::set<Fact> original(graph.edges.begin(), graph.edges.end());
    for (const auto &f : original)
      if (!original.count(
              {f.target, options.reverse_symbols[f.symbol], f.source}))
        throw std::invalid_argument(
            "bidirected input graph lacks a reverse terminal edge");
  }
  Result result;
  std::optional<Symbol> chosen;
  for (std::size_t i = 0; i < grammar.terminal.size(); ++i) {
    const auto a = static_cast<Symbol>(i);
    auto c = check(grammar, a, input.outputs, options.direction,
                   options.reverse_symbols);
    if (!c.eligible)
      continue;
    result.admissible_symbols.push_back(a);
    if (!chosen && (options.variant == Variant::Iea || c.doubly_recursive))
      chosen = a;
  }
  if (options.transitive_symbol) {
    const auto c = check(grammar, *options.transitive_symbol, input.outputs,
                         options.direction, options.reverse_symbols);
    if (!c.eligible)
      throw std::invalid_argument(
          "unsafe transitive symbol for the requested result contract");
    if (options.variant == Variant::IeaOcr && !c.doubly_recursive)
      throw std::invalid_argument("IeaOcr additionally requires A ::= A A");
    chosen = *options.transitive_symbol;
  }
  if (!chosen && options.require_optimization)
    throw std::invalid_argument("no admissible transitive symbol");
  const bool ordered = chosen && options.variant == Variant::IeaOcr;
  Kernel kernel(grammar, input, options, chosen, ordered);
  kernel.run();
  result.reachability = kernel.result();
  result.stats = kernel.stats;
  result.epoch_trace = std::move(kernel.traces);
  result.transitive_symbol = chosen;
  return result;
}
} // namespace lotus::cfl::classical::ieoce
