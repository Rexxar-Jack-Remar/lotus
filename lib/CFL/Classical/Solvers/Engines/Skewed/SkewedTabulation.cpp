// SPDX-License-Identifier: MIT
#include "CFL/Classical/Solvers/Engines/Skewed/SkewedTabulation.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::classical::skewed {
namespace {

std::vector<Rule> canonicalRules(const std::vector<Rule> &input) {
  std::vector<Rule> result = input;
  for (auto &r : result) {
    if (r.arity == 0)
      r.first = 0;
    if (r.arity < 2)
      r.second = 0;
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<Symbol> outputs(const Grammar &g, const Options &o) {
  std::vector<Symbol> result;
  if (o.scope == Scope::AllSymbols) {
    if (!o.targets.empty())
      throw std::invalid_argument("targets require Scope::TargetsOnly");
    result.resize(g.terminal.size());
    std::iota(result.begin(), result.end(), Symbol{0});
  } else {
    result = o.targets.empty() ? std::vector<Symbol>{g.start} : o.targets;
    for (Symbol s : result)
      if (s >= g.terminal.size() || g.terminal[s])
        throw std::invalid_argument("a target must be a declared nonterminal");
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
  }
  return result;
}

bool mentions(const Rule &r, Symbol c) {
  return (r.arity >= 1 && r.first == c) || (r.arity == 2 && r.second == c);
}

// Reject C when another nonterminal shares its dependency SCC. This avoids
// applying a one-variable recursion identity to a mutually recursive system.
// The search is iterative (no C++ recursion / input-dependent stack depth).
bool indirectlyRecursive(const Grammar &g, Symbol c) {
  std::vector<std::vector<Symbol>> next(g.terminal.size());
  for (const auto &r : g.rules) {
    if (r.arity >= 1 && !g.terminal[r.first])
      next[r.lhs].push_back(r.first);
    if (r.arity == 2 && !g.terminal[r.second])
      next[r.lhs].push_back(r.second);
  }
  std::vector<bool> seen(g.terminal.size(), false);
  std::vector<Symbol> stack;
  for (Symbol s : next[c])
    if (s != c)
      stack.push_back(s);
  while (!stack.empty()) {
    Symbol s = stack.back();
    stack.pop_back();
    if (s == c)
      return true;
    if (seen[s])
      continue;
    seen[s] = true;
    for (Symbol t : next[s])
      stack.push_back(t);
  }
  return false;
}

struct Consumer {
  Symbol symbol;
  bool suffix; // Every original production is A -> P C, versus A -> C P.
};

// This intentionally rejects consumers that have unrelated alternatives.
// For example A -> p C | q is NOT safely closed under C's recursion.
bool consumersFor(const Grammar &g, Symbol c, RewriteKind kind,
                  std::vector<Consumer> &consumers) {
  std::set<Symbol> lhs;
  for (const auto &r : g.rules) {
    if (r.lhs == c || !mentions(r, c))
      continue;
    if (r.arity != 2 || (r.first == c && r.second == c))
      return false;
    lhs.insert(r.lhs);
  }
  for (Symbol a : lhs) {
    bool suffix = true, prefix = true;
    for (const auto &r : g.rules)
      if (r.lhs == a) {
        suffix = suffix && r.arity == 2 && r.second == c && r.first != c;
        prefix = prefix && r.arity == 2 && r.first == c && r.second != c;
      }
    if (kind == RewriteKind::LeftRecursionTransfer)
      prefix = false;
    if (kind == RewriteKind::RightRecursionTransfer)
      suffix = false;
    if (!suffix && !prefix)
      return false;
    consumers.push_back({a, suffix});
  }
  return !consumers.empty();
}

bool transformOne(Grammar &g, Symbol c, Rewrite &event) {
  if (indirectlyRecursive(g, c))
    return false;
  bool left = false, right = false, transitive = false;
  std::vector<Symbol> append;
  bool epsilon = false;
  for (const auto &r : g.rules)
    if (r.lhs == c) {
      epsilon = epsilon || r.arity == 0;
      if (!mentions(r, c))
        continue;
      if (r.arity == 1)
        return false; // Conservative: do not rewrite C -> C.
      if (r.first == c && r.second == c)
        transitive = true;
      else if (r.first == c) {
        left = true;
        append.push_back(r.second);
      } else {
        right = true;
        append.push_back(r.first);
      }
    }
  if (static_cast<unsigned>(left) + static_cast<unsigned>(right) +
          static_cast<unsigned>(transitive) !=
      1)
    return false;
  const RewriteKind kind = transitive ? RewriteKind::TransitiveRecursionTransfer
                           : left     ? RewriteKind::LeftRecursionTransfer
                                      : RewriteKind::RightRecursionTransfer;
  std::vector<Consumer> consumers;
  if (!consumersFor(g, c, kind, consumers))
    return false;

  const auto original = g.rules;
  std::vector<Rule> changed;
  std::vector<int> orientation(g.terminal.size(), 0);
  for (const auto &a : consumers)
    orientation[a.symbol] = a.suffix ? 1 : -1;
  for (const auto &r : original) {
    if (r.lhs == c && mentions(r, c))
      continue;
    // C = B* in this case: pull the nullable base into A's seed, then
    // obtain all nonempty repetitions via the newly added A recursion.
    if (transitive && epsilon && r.lhs == c && r.arity == 0)
      continue;
    if (transitive && epsilon && r.lhs != c && mentions(r, c)) {
      changed.push_back(
          Rule::unary(r.lhs, orientation[r.lhs] == 1 ? r.first : r.second));
    } else {
      changed.push_back(r);
    }
  }
  for (const auto &a : consumers) {
    if (transitive) {
      changed.push_back(a.suffix ? Rule::binary(a.symbol, a.symbol, c)
                                 : Rule::binary(a.symbol, c, a.symbol));
    } else {
      for (Symbol x : append)
        changed.push_back(a.suffix ? Rule::binary(a.symbol, a.symbol, x)
                                   : Rule::binary(a.symbol, x, a.symbol));
    }
  }

  std::size_t inlined = 0;
  // Restricted Algorithm-2 terminal-base elimination. After the nullable
  // transitive rewrite, all remaining C uses are singly recursive. Replace
  // C -> t with the corresponding terminal recursion in EVERY consumer.
  // Nonterminal unary/binary bases remain intact.
  if (transitive && epsilon) {
    std::vector<Rule> kept;
    std::vector<Symbol> terminals;
    for (const auto &r : changed) {
      if (r.lhs == c && r.arity == 1 && g.terminal[r.first]) {
        terminals.push_back(r.first);
        ++inlined;
      } else
        kept.push_back(r);
    }
    for (const auto &a : consumers)
      for (Symbol t : terminals)
        kept.push_back(a.suffix ? Rule::binary(a.symbol, a.symbol, t)
                                : Rule::binary(a.symbol, t, a.symbol));
    changed = std::move(kept);
  }
  changed = canonicalRules(changed);
  event = {kind, c, {}, 0, 0, transitive && epsilon ? 1U : 0U, inlined};
  for (const auto &a : consumers)
    event.consumers.push_back(a.symbol);
  for (const auto &r : original)
    if (!std::binary_search(changed.begin(), changed.end(), r))
      ++event.removed_rules;
  for (const auto &r : changed)
    if (!std::binary_search(original.begin(), original.end(), r))
      ++event.added_rules;
  g.rules = std::move(changed);
  return true;
}

using Vertex = std::uint32_t;
struct DenseFact {
  Vertex source;
  Symbol symbol;
  Vertex target;
  bool operator==(const DenseFact &f) const {
    return source == f.source && symbol == f.symbol && target == f.target;
  }
};

std::size_t mixedHash(std::uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return static_cast<std::size_t>(x);
}
struct DenseHash {
  std::size_t operator()(const DenseFact &f) const {
    auto h = mixedHash((std::uint64_t{f.source} << 32) | f.target);
    return h ^
           mixedHash(std::uint64_t{f.symbol} + UINT64_C(0x9e3779b97f4a7c15));
  }
};

struct Row {
  std::unordered_set<Vertex> members;
  std::vector<Vertex> entries; // Stable row, append-only entries.
};
std::uint64_t rowKey(Symbol symbol, Vertex node) {
  return (std::uint64_t{symbol} << 32) | node;
}

// E is a two-way adjacency relation. PE, below, is deliberately NOT indexed.
class IndexedRelation {
public:
  bool contains(const DenseFact &f) const {
    const auto *row = successors(f.symbol, f.source);
    return row && row->members.count(f.target) != 0;
  }
  bool insert(const DenseFact &f) {
    auto &out = forward_[rowKey(f.symbol, f.source)];
    if (!out.members.insert(f.target).second)
      return false;
    out.entries.push_back(f.target);
    auto &in = reverse_[rowKey(f.symbol, f.target)];
    in.members.insert(f.source);
    in.entries.push_back(f.source);
    facts_.push_back(f);
    return true;
  }
  const Row *successors(Symbol s, Vertex v) const {
    auto it = forward_.find(rowKey(s, v));
    return it == forward_.end() ? nullptr : &it->second;
  }
  const Row *predecessors(Symbol s, Vertex v) const {
    auto it = reverse_.find(rowKey(s, v));
    return it == reverse_.end() ? nullptr : &it->second;
  }
  const std::vector<DenseFact> &facts() const { return facts_; }

private:
  std::unordered_map<std::uint64_t, Row> forward_, reverse_;
  std::vector<DenseFact> facts_;
};

struct Compiled {
  std::vector<std::vector<Symbol>> unary;
  std::vector<std::vector<Rule>> left, right;
  std::vector<Symbol> epsilon;
  std::vector<bool> pn, dynamic;

  Compiled(const Grammar &g, const Options &o, const std::vector<Symbol> &out)
      : unary(g.terminal.size()), left(g.terminal.size()),
        right(g.terminal.size()), pn(g.terminal.size(), false),
        dynamic(g.terminal.size(), false) {
    std::vector<bool> protected_symbol(g.terminal.size(), false);
    for (Symbol s : out)
      protected_symbol[s] = true;
    if (o.propagating_symbols) {
      for (Symbol s : *o.propagating_symbols) {
        if (s >= g.terminal.size() || g.terminal[s])
          throw std::invalid_argument(
              "a propagating symbol must be a declared nonterminal");
        if (protected_symbol[s])
          throw std::invalid_argument(
              "a requested output cannot be a propagating symbol");
        pn[s] = o.static_propagating_edges;
      }
    }
    for (std::size_t s = 0; s < g.terminal.size(); ++s) {
      if (!o.propagating_symbols)
        pn[s] = !g.terminal[s] && !protected_symbol[s] &&
                o.static_propagating_edges;
      dynamic[s] = !g.terminal[s] && o.dynamic_transitive_edges;
    }
    std::vector<bool> has_transitive(g.terminal.size(), false);
    for (const auto &r : g.rules) {
      if (r.arity == 0)
        epsilon.push_back(r.lhs);
      else if (r.arity == 1)
        unary[r.first].push_back(r.lhs);
      else {
        left[r.first].push_back(r);
        right[r.second].push_back(r);
        // Algorithm 2, PN test: the sibling is terminal and the LHS is
        // different from the candidate. Unary uses do not disqualify it.
        if (!o.propagating_symbols) {
          if (!g.terminal[r.second] || r.lhs == r.first)
            pn[r.first] = false;
          if (!g.terminal[r.first] || r.lhs == r.second)
            pn[r.second] = false;
        }
        // Algorithm 3, dynamic test: every binary use must be recursive in
        // its other operand. This is NOT merely "X has an X -> X X rule".
        if (r.lhs != r.second)
          dynamic[r.first] = false;
        if (r.lhs != r.first)
          dynamic[r.second] = false;
        if (r.lhs == r.first && r.lhs == r.second)
          has_transitive[r.lhs] = true;
      }
    }
    for (std::size_t s = 0; s < dynamic.size(); ++s)
      dynamic[s] = dynamic[s] && has_transitive[s];
  }
};

class Kernel {
public:
  Kernel(const Grammar &g, const Options &o, const std::vector<Symbol> &out,
         const std::vector<Node> &nodes)
      : rules_(g, o, out), options_(o), nodes_(nodes) {
    stats.nodes = nodes.size();
    stats.propagating_symbols = static_cast<std::size_t>(
        std::count(rules_.pn.begin(), rules_.pn.end(), true));
    stats.dynamic_eligible_symbols = static_cast<std::size_t>(
        std::count(rules_.dynamic.begin(), rules_.dynamic.end(), true));
  }
  void run(const Graph &graph) {
    std::unordered_map<Node, Vertex> id;
    for (std::size_t i = 0; i < nodes_.size(); ++i)
      id.emplace(nodes_[i], static_cast<Vertex>(i));
    // ALL terminals must be in E before any work item is processed.
    for (const auto &f : graph.edges) {
      if (insertIndexed({id.at(f.source), f.symbol, id.at(f.target)}))
        ++stats.unique_base_edges;
    }
    // Only explicit epsilon productions seed E. Nullable consequences are
    // derived normally, including epsilon/unit cycles and binary nullability.
    for (Symbol s : rules_.epsilon)
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
        auto v = static_cast<Vertex>(i);
        insertIndexed({v, s, v});
      }

    while (!work_.empty()) {
      const DenseFact f = work_.front();
      work_.pop_front();
      ++stats.work_items;
      for (Symbol lhs : rules_.unary[f.symbol]) {
        ++stats.unary_applications;
        update({f.source, lhs, f.target}, false);
      }
      for (const auto &r : rules_.left[f.symbol]) {
        const Row *row = indexed.successors(r.second, f.target);
        if (!row)
          continue;
        const auto end = row->entries.size();
        // Do NOT retain iterators or references into entries across update().
        // unordered_map rehash preserves Row pointers, and each vertex value
        // is copied before any push_back can reallocate its entries vector.
        for (std::size_t i = 0; i < end; ++i) {
          const Vertex target = row->entries[i];
          ++stats.binary_join_pairs;
          update({f.source, r.lhs, target},
                 r.lhs == r.first && r.lhs == r.second);
        }
      }
      for (const auto &r : rules_.right[f.symbol]) {
        const Row *row = indexed.predecessors(r.first, f.source);
        if (!row)
          continue;
        const auto end = row->entries.size();
        for (std::size_t i = 0; i < end; ++i) {
          const Vertex source = row->entries[i];
          ++stats.binary_join_pairs;
          update({source, r.lhs, f.target},
                 r.lhs == r.first && r.lhs == r.second);
        }
      }
    }
    stats.indexed_facts = indexed.facts().size();
    stats.propagating_facts = pe.size();
  }

  IndexedRelation indexed;
  std::unordered_set<DenseFact, DenseHash> pe;
  Stats stats;

private:
  void checkLimit() const {
    if (options_.max_fact_entries != 0 &&
        (indexed.facts().size() >= options_.max_fact_entries ||
         pe.size() >= options_.max_fact_entries - indexed.facts().size()))
      throw ResourceLimit("skewed tabulation E + PE entry limit exceeded");
  }
  void enqueue(const DenseFact &f) {
    work_.push_back(f);
    stats.peak_worklist = std::max(stats.peak_worklist, work_.size());
  }
  bool insertIndexed(const DenseFact &f) {
    ++stats.attempts;
    if (indexed.contains(f)) {
      ++stats.duplicate_attempts;
      return false;
    }
    checkLimit();
    if (pe.count(f))
      ++stats.promotions_to_indexed;
    indexed.insert(f);
    enqueue(f);
    return true;
  }
  void update(const DenseFact &f, bool transitive_derivation) {
    const bool static_pe = rules_.pn[f.symbol];
    const bool dynamic_pe =
        !static_pe && transitive_derivation && rules_.dynamic[f.symbol];
    if (!static_pe && !dynamic_pe) {
      // Check E, not E union PE. A previous propagating derivation must NOT
      // suppress insertion into E when an ordinary derivation appears later.
      insertIndexed(f);
      return;
    }
    ++stats.attempts;
    if (pe.count(f)) {
      ++stats.duplicate_attempts;
      return;
    }
    checkLimit();
    pe.insert(f);
    if (static_pe)
      ++stats.static_pe_insertions;
    else
      ++stats.dynamic_pe_insertions;
    enqueue(f);
  }
  Compiled rules_;
  const Options &options_;
  const std::vector<Node> &nodes_;
  std::deque<DenseFact> work_;
};

} // namespace

void Grammar::validate() const {
  if (terminal.empty() || terminal.size() > std::numeric_limits<Symbol>::max())
    throw std::invalid_argument("invalid number of grammar symbols");
  if (start >= terminal.size() || terminal[start])
    throw std::invalid_argument("start must be a declared nonterminal");
  for (const auto &r : rules) {
    if (r.arity > 2)
      throw std::invalid_argument("grammar is not normalized");
    if (r.lhs >= terminal.size() || terminal[r.lhs])
      throw std::invalid_argument("production LHS must be a nonterminal");
    if ((r.arity >= 1 && r.first >= terminal.size()) ||
        (r.arity == 2 && r.second >= terminal.size()))
      throw std::invalid_argument("undeclared production RHS symbol");
  }
}

Transformation
skewConservatively(const Grammar &grammar,
                   const std::vector<Symbol> &protected_symbols) {
  grammar.validate();
  Transformation t{grammar, {}};
  t.grammar.rules = canonicalRules(t.grammar.rules);
  std::vector<bool> unavailable(grammar.terminal.size(), false);
  for (Symbol s : protected_symbols) {
    if (s >= grammar.terminal.size())
      throw std::invalid_argument("undeclared protected symbol");
    unavailable[s] = true;
  }
  for (std::size_t i = 0; i < grammar.terminal.size(); ++i)
    unavailable[i] = unavailable[i] || grammar.terminal[i];

  // At most one successful rewrite per symbol: bounded termination, even if
  // another rewrite later creates a new opportunity for a processed symbol.
  bool progress;
  do {
    progress = false;
    for (std::size_t i = 0; i < unavailable.size(); ++i) {
      if (unavailable[i])
        continue;
      Rewrite event{};
      if (transformOne(t.grammar, static_cast<Symbol>(i), event)) {
        unavailable[i] = true;
        t.rewrites.push_back(std::move(event));
        progress = true;
      }
    }
  } while (progress);
  return t;
}

bool Result::coversSymbol(Symbol s) const {
  return std::binary_search(output_symbols_.begin(), output_symbols_.end(), s);
}
bool Result::contains(Node source, Symbol symbol, Node target) const {
  if (!coversSymbol(symbol))
    throw std::invalid_argument(
        "relation was not requested / is not guaranteed");
  return std::binary_search(facts_.begin(), facts_.end(),
                            Fact{source, symbol, target});
}

Result solve(const Grammar &grammar, const Graph &graph,
             const Options &options) {
  grammar.validate();
  const auto out = outputs(grammar, options);
  for (const auto &f : graph.edges)
    if (f.symbol >= grammar.terminal.size() || !grammar.terminal[f.symbol])
      throw std::invalid_argument(
          "input graph may contain only declared terminals");

  Result result;
  result.output_symbols_ = out;
  result.nodes_ = graph.nodes;
  for (const auto &f : graph.edges) {
    result.nodes_.push_back(f.source);
    result.nodes_.push_back(f.target);
  }
  std::sort(result.nodes_.begin(), result.nodes_.end());
  result.nodes_.erase(std::unique(result.nodes_.begin(), result.nodes_.end()),
                      result.nodes_.end());
  if (result.nodes_.size() > std::numeric_limits<Vertex>::max())
    throw ResourceLimit("more than UINT32_MAX distinct vertices");

  if (options.static_mode == StaticMode::Conservative) {
    auto t = skewConservatively(grammar, out);
    result.working_grammar_ = std::move(t.grammar);
    result.rewrites_ = std::move(t.rewrites);
  } else {
    result.working_grammar_ = grammar;
    result.working_grammar_.rules = canonicalRules(grammar.rules);
  }
  Kernel kernel(result.working_grammar_, options, out, result.nodes_);
  kernel.run(graph);
  std::vector<bool> selected(grammar.terminal.size(), false);
  for (Symbol s : out)
    selected[s] = true;
  auto emit = [&](const DenseFact &f) {
    if (selected[f.symbol])
      result.facts_.push_back(
          {result.nodes_[f.source], f.symbol, result.nodes_[f.target]});
  };
  for (const auto &f : kernel.indexed.facts())
    emit(f);
  for (const auto &f : kernel.pe)
    emit(f);
  std::sort(result.facts_.begin(), result.facts_.end());
  result.facts_.erase(std::unique(result.facts_.begin(), result.facts_.end()),
                      result.facts_.end());
  result.stats_ = kernel.stats;
  result.stats_.output_facts = result.facts_.size();
  return result;
}

RebuildingSession::RebuildingSession(Grammar grammar, Graph graph,
                                     Options options)
    : grammar_(std::move(grammar)), graph_(std::move(graph)),
      options_(std::move(options)) {
  grammar_.validate();
  (void)outputs(grammar_, options_);
  for (Node v : graph_.nodes)
    known_nodes_.insert(v);
  for (const auto &f : graph_.edges) {
    if (f.symbol >= grammar_.terminal.size() || !grammar_.terminal[f.symbol])
      throw std::invalid_argument(
          "input graph may contain only declared terminals");
    known_edges_.insert(f);
    known_nodes_.insert(f.source);
    known_nodes_.insert(f.target);
  }
}

bool RebuildingSession::addNode(Node node) {
  if (known_nodes_.count(node))
    return false;
  // Roll back the auxiliary set if vector allocation fails.
  auto inserted = known_nodes_.insert(node);
  try {
    graph_.nodes.push_back(node);
  } catch (...) {
    known_nodes_.erase(inserted.first);
    throw;
  }
  return true;
}

bool RebuildingSession::addTerminalEdge(Node source, Symbol symbol,
                                        Node target) {
  if (symbol >= grammar_.terminal.size() || !grammar_.terminal[symbol])
    throw std::invalid_argument(
        "incremental edge must have a declared terminal label");
  const Fact f{source, symbol, target};
  if (known_edges_.count(f))
    return false;
  // Fully transactional with respect to allocation failures. Endpoint vertices
  // are implicit in edges, so no explicit graph_.nodes entries are required.
  const bool new_source = known_nodes_.count(source) == 0;
  const bool new_target = target != source && known_nodes_.count(target) == 0;
  bool source_inserted = false, target_inserted = false, edge_inserted = false;
  try {
    if (new_source) {
      known_nodes_.insert(source);
      source_inserted = true;
    }
    if (new_target) {
      known_nodes_.insert(target);
      target_inserted = true;
    }
    known_edges_.insert(f);
    edge_inserted = true;
    graph_.edges.push_back(f);
  } catch (...) {
    if (edge_inserted)
      known_edges_.erase(f);
    if (target_inserted)
      known_nodes_.erase(target);
    if (source_inserted)
      known_nodes_.erase(source);
    throw;
  }
  return true;
}

void RebuildingSession::solve() {
  Result next = skewed::solve(grammar_, graph_, options_);
  result_ = std::move(next);
}

const Result &RebuildingSession::result() const {
  if (!result_)
    throw std::logic_error("solve() is required before relation queries");
  return *result_;
}

} // namespace lotus::cfl::classical::skewed
