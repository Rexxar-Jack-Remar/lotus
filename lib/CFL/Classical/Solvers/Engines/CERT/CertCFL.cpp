#include "CFL/Classical/Solvers/Engines/CERT/CertCFL.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <numeric>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::classical::engines::cert {
namespace {

std::size_t mix(std::size_t h, std::size_t v) noexcept {
  return h ^ (v + std::size_t{0x9e3779b9U} + (h << 6U) + (h >> 2U));
}
std::size_t checkedAdd(std::size_t a, std::size_t b) {
  if (b > std::numeric_limits<std::size_t>::max() - a)
    throw std::overflow_error("CERT-CFL cardinality/size overflow");
  return a + b;
}
std::size_t checkedMul(std::size_t a, std::size_t b) {
  if (a && b > std::numeric_limits<std::size_t>::max() / a)
    throw std::overflow_error("CERT-CFL cardinality/size overflow");
  return a * b;
}
std::size_t threshold(std::size_t value, std::size_t size, bool majority) {
  if (size == 0 || value > size)
    throw std::logic_error("CERT-CFL invalid degree bound");
  if (value == size)
    return size;
  const auto half = size / 2 + 1;
  if (majority && value >= half)
    return half;
  return value ? 1 : 0;
}
struct Triple {
  std::size_t a, b, c;
  bool operator==(const Triple &o) const noexcept {
    return a == o.a && b == o.b && c == o.c;
  }
};
struct TripleHash {
  std::size_t operator()(const Triple &k) const noexcept {
    return mix(mix(k.a, k.b), k.c);
  }
};
struct Tile {
  std::size_t out = 0, in = 0;
  bool may = false, queued = false;
};
using Adjacency = std::unordered_map<Node, std::vector<Node>>;
struct Data {
  std::size_t nodes = 0, symbols = 0;
  Partition groups;
  std::vector<std::size_t> owner;
  std::vector<Tile> tiles;
  std::vector<std::uint8_t> nullable, exact, sparse_symbol;
  // In these maps nullable diagonals are omitted and remain symbolic.
  std::vector<Adjacency> successors, predecessors;
  std::vector<std::size_t> counts;
  Statistics stats;
  std::size_t index(Symbol a, std::size_t i, std::size_t j) const {
    return (std::size_t{a} * groups.size() + i) * groups.size() + j;
  }
  bool full(Symbol a, std::size_t i, std::size_t j) const {
    return tiles[index(a, i, j)].out == groups[j].size();
  }
  void requireSymbol(Symbol a) const {
    if (a >= symbols)
      throw std::out_of_range("CERT-CFL symbol out of range");
  }
  void requireNode(Node u) const {
    if (u >= nodes)
      throw std::out_of_range("CERT-CFL node out of range");
  }
  void requireExact(Symbol a) const {
    requireSymbol(a);
    if (!exact[a])
      throw std::logic_error("CERT-CFL requested an unresolved symbol");
  }
};
struct Prepared {
  std::size_t nodes = 0, symbols = 0;
  std::vector<Seed> seeds;
  std::vector<UnaryRule> unary;
  std::vector<BinaryRule> binary;
  std::vector<std::uint8_t> nullable, direct;
  std::vector<std::vector<Symbol>> units;
  std::vector<std::vector<std::pair<Symbol, Symbol>>> left, right;
  std::vector<Symbol> observed;
};

Prepared prepare(const Problem &p, const Options &o) {
  // Keep Symbol loops representable without wraparound at UINT32_MAX+1.
  if (p.symbols > std::numeric_limits<Symbol>::max())
    throw std::invalid_argument("CERT-CFL too many grammar symbols");
  auto valid = [&](Symbol a) {
    if (a >= p.symbols)
      throw std::invalid_argument("CERT-CFL invalid grammar symbol");
  };
  for (const auto &e : p.seeds) {
    valid(e.symbol);
    if (e.source >= p.nodes || e.target >= p.nodes)
      throw std::invalid_argument("CERT-CFL invalid seed endpoint");
  }
  for (const auto &r : p.unary) { valid(r.lhs); valid(r.rhs); }
  for (const auto &r : p.binary) {
    valid(r.lhs); valid(r.first); valid(r.second);
  }
  for (Symbol a : p.epsilon) valid(a);
  Prepared q;
  q.nodes = p.nodes; q.symbols = p.symbols;
  q.seeds = p.seeds;
  std::sort(q.seeds.begin(), q.seeds.end(), [](const Seed &a, const Seed &b) {
    return std::tie(a.symbol, a.source, a.target) <
           std::tie(b.symbol, b.source, b.target);
  });
  q.seeds.erase(std::unique(q.seeds.begin(), q.seeds.end()), q.seeds.end());
  q.unary = p.unary; q.binary = p.binary;
  std::sort(q.binary.begin(), q.binary.end(), [](const BinaryRule &a, const BinaryRule &b) {
    return std::tie(a.lhs, a.first, a.second) < std::tie(b.lhs, b.first, b.second);
  });
  q.binary.erase(std::unique(q.binary.begin(), q.binary.end(), [](const BinaryRule &a, const BinaryRule &b) {
    return std::tie(a.lhs, a.first, a.second) == std::tie(b.lhs, b.first, b.second);
  }), q.binary.end());

  q.nullable.assign(q.symbols, 0);
  std::vector<std::vector<std::size_t>> uses(q.symbols);
  std::vector<unsigned> remaining;
  std::vector<Symbol> heads;
  auto dependency = [&](Symbol head, Symbol first, std::optional<Symbol> second) {
    const auto id = remaining.size();
    remaining.push_back(second ? 2U : 1U); heads.push_back(head);
    uses[first].push_back(id);
    // If first == second, both occurrences MUST be recorded.
    if (second) uses[*second].push_back(id);
  };
  for (const auto &r : q.unary) dependency(r.lhs, r.rhs, std::nullopt);
  for (const auto &r : q.binary) dependency(r.lhs, r.first, r.second);
  std::deque<Symbol> work;
  for (Symbol a : p.epsilon)
    if (!q.nullable[a]) { q.nullable[a] = 1; work.push_back(a); }
  while (!work.empty()) {
    const Symbol child = work.front(); work.pop_front();
    for (auto id : uses[child])
      if (--remaining[id] == 0 && !q.nullable[heads[id]]) {
        q.nullable[heads[id]] = 1; work.push_back(heads[id]);
      }
  }
  for (const auto &r : q.binary) {
    if (q.nullable[r.first]) q.unary.push_back({r.lhs, r.second});
    if (q.nullable[r.second]) q.unary.push_back({r.lhs, r.first});
  }
  std::sort(q.unary.begin(), q.unary.end(), [](const UnaryRule &a, const UnaryRule &b) {
    return std::tie(a.lhs, a.rhs) < std::tie(b.lhs, b.rhs);
  });
  q.unary.erase(std::unique(q.unary.begin(), q.unary.end(), [](const UnaryRule &a, const UnaryRule &b) {
    return a.lhs == b.lhs && a.rhs == b.rhs;
  }), q.unary.end());
  q.units.resize(q.symbols); q.left.resize(q.symbols); q.right.resize(q.symbols);
  q.direct.assign(q.symbols, o.keep_seed_symbols_explicit ? 1 : 0);
  for (const auto &r : q.unary) {
    q.units[r.rhs].push_back(r.lhs); q.direct[r.lhs] = 0;
  }
  for (const auto &r : q.binary) {
    q.left[r.first].emplace_back(r.lhs, r.second);
    q.right[r.second].emplace_back(r.lhs, r.first);
    q.direct[r.lhs] = 0;
  }
  if (o.observed) q.observed = *o.observed;
  else {
    q.observed.resize(q.symbols);
    std::iota(q.observed.begin(), q.observed.end(), Symbol{0});
  }
  for (Symbol a : q.observed) valid(a);
  std::sort(q.observed.begin(), q.observed.end());
  q.observed.erase(std::unique(q.observed.begin(), q.observed.end()), q.observed.end());
  return q;
}
Partition initialPartition(std::size_t n, const Partition &input) {
  Partition groups = input;
  if (groups.empty() && n) {
    groups.resize(1); groups[0].resize(n);
    std::iota(groups[0].begin(), groups[0].end(), Node{0});
  }
  std::vector<std::uint8_t> seen(n, 0);
  std::size_t covered = 0;
  for (const auto &g : groups) {
    if (g.empty()) throw std::invalid_argument("CERT-CFL empty partition block");
    for (Node u : g) {
      if (u >= n || seen[u])
        throw std::invalid_argument("CERT-CFL invalid/duplicate partition vertex");
      seen[u] = 1; ++covered;
    }
  }
  if (covered != n) throw std::invalid_argument("CERT-CFL incomplete partition");
  return groups;
}
Partition refine(const Partition &groups) {
  Partition next;
  for (const auto &g : groups) {
    if (g.size() == 1) next.push_back({g.front()});
    else {
      const auto mid = g.begin() + static_cast<std::ptrdiff_t>(g.size() / 2);
      next.emplace_back(g.begin(), mid); next.emplace_back(mid, g.end());
    }
  }
  return next;
}
void addSparseSeed(Data &d, const Seed &e) {
  if (d.nullable[e.symbol] && e.source == e.target) return;
  d.successors[e.symbol][e.source].push_back(e.target);
  d.predecessors[e.symbol][e.target].push_back(e.source);
}
void finish(Data &d) {
  d.exact.assign(d.symbols, 0); d.counts.assign(d.symbols, 0);
  for (std::size_t a = 0; a < d.symbols; ++a) {
    if (d.sparse_symbol[a]) {
      d.exact[a] = 1;
      std::size_t count = d.nullable[a] ? d.nodes : 0;
      for (auto &row : d.successors[a]) {
        if (!std::is_sorted(row.second.begin(), row.second.end()))
          std::sort(row.second.begin(), row.second.end());
        count = checkedAdd(count, row.second.size());
      }
      for (auto &row : d.predecessors[a])
        if (!std::is_sorted(row.second.begin(), row.second.end()))
          std::sort(row.second.begin(), row.second.end());
      d.counts[a] = count;
      continue;
    }
    bool exact = true;
    std::size_t count = 0;
    for (std::size_t i = 0; i < d.groups.size(); ++i)
      for (std::size_t j = 0; j < d.groups.size(); ++j) {
        const auto &t = d.tiles[d.index(static_cast<Symbol>(a), i, j)];
        if (t.out == d.groups[j].size())
          count = checkedAdd(count, checkedMul(d.groups[i].size(), d.groups[j].size()));
        else {
          if (t.may) exact = false;
          if (d.nullable[a] && i == j) count = checkedAdd(count, d.groups[i].size());
        }
      }
    d.exact[a] = exact ? 1 : 0;
    if (exact) d.counts[a] = count;
  }
}

class DenseLevel {
public:
  DenseLevel(const Prepared &p, const Options &o, Partition groups,
             Statistics &stats, std::size_t tiles)
      : p_(p), o_(o), stats_(stats) {
    d_.nodes = p.nodes; d_.symbols = p.symbols; d_.groups = std::move(groups);
    b_ = d_.groups.size(); d_.owner.resize(p.nodes); d_.tiles.resize(tiles);
    d_.nullable = p.nullable; d_.sparse_symbol = p.direct;
    d_.successors.resize(p.symbols); d_.predecessors.resize(p.symbols);
    for (std::size_t i = 0; i < b_; ++i)
      for (Node u : d_.groups[i]) d_.owner[u] = i;
    for (const auto &e : p.seeds)
      if (p.direct[e.symbol]) addSparseSeed(d_, e);
  }
  Data run() {
    initialize();
    while (!queue_.empty()) {
      const auto id = queue_.front(); queue_.pop_front();
      d_.tiles[id].queued = false; ++stats_.queue_pops;
      const auto j = id % b_, i = (id / b_) % b_;
      const auto a = static_cast<Symbol>(id / b_ / b_);
      // Copy before applying rules: output and input may alias under recursion.
      const Tile selected = d_.tiles[id];
      for (Symbol head : p_.units[a])
        update(head, i, j, selected.may, selected.out, selected.in);
      for (const auto &rule : p_.left[a])
        for (std::size_t target = 0; target < b_; ++target)
          join(rule.first, a, rule.second, i, j, target);
      for (const auto &rule : p_.right[a])
        for (std::size_t source = 0; source < b_; ++source)
          join(rule.first, rule.second, a, source, i, j);
    }
    finish(d_);
    return std::move(d_);
  }
private:
  void update(Symbol a, std::size_t i, std::size_t j, bool may,
              std::size_t out, std::size_t in) {
    const auto id = d_.index(a, i, j);
    Tile &t = d_.tiles[id];
    out = std::max(t.out, threshold(out, d_.groups[j].size(), o_.majority_threshold));
    in = std::max(t.in, threshold(in, d_.groups[i].size(), o_.majority_threshold));
    if (out == d_.groups[j].size() || in == d_.groups[i].size()) {
      out = d_.groups[j].size(); in = d_.groups[i].size();
    }
    may = may || t.may;
    if (t.may == may && t.out == out && t.in == in) return;
    t.may = may; t.out = out; t.in = in; ++stats_.updates;
    if (!t.queued) {
      queue_.push_back(id); t.queued = true;
      stats_.peak_queue = std::max(stats_.peak_queue, queue_.size());
    }
  }
  void initialize() {
    std::unordered_map<Triple, std::size_t, TripleHash> rows, cols;
    auto lowerSeed = [&](Symbol a, Node u, Node v) {
      ++rows[Triple{a, u, d_.owner[v]}];
      ++cols[Triple{a, d_.owner[u], v}];
    };
    // p_.seeds is already deduplicated. Nullable self-loops are counted once,
    // as symbolic identities below, but still seed MAY here.
    for (const auto &e : p_.seeds) {
      update(e.symbol, d_.owner[e.source], d_.owner[e.target], true, 0, 0);
      if (!(p_.nullable[e.symbol] && e.source == e.target))
        lowerSeed(e.symbol, e.source, e.target);
    }
    for (std::size_t a = 0; a < p_.symbols; ++a)
      if (p_.nullable[a])
        for (Node u = 0; u < p_.nodes; ++u)
          lowerSeed(static_cast<Symbol>(a), u, u);
    struct Minimum { std::size_t covered = 0, minimum = std::numeric_limits<std::size_t>::max(); };
    std::unordered_map<std::size_t, Minimum> rowMin, colMin;
    for (const auto &entry : rows) {
      const auto &k = entry.first;
      auto &m = rowMin[d_.index(static_cast<Symbol>(k.a), d_.owner[k.b], k.c)];
      ++m.covered; m.minimum = std::min(m.minimum, entry.second);
    }
    for (const auto &entry : cols) {
      const auto &k = entry.first;
      auto &m = colMin[d_.index(static_cast<Symbol>(k.a), k.b, d_.owner[k.c])];
      ++m.covered; m.minimum = std::min(m.minimum, entry.second);
    }
    for (const auto &entry : rowMin) {
      const auto id = entry.first, j = id % b_, i = (id / b_) % b_;
      if (entry.second.covered == d_.groups[i].size())
        update(static_cast<Symbol>(id / b_ / b_), i, j, false, entry.second.minimum, 0);
    }
    for (const auto &entry : colMin) {
      const auto id = entry.first, j = id % b_, i = (id / b_) % b_;
      if (entry.second.covered == d_.groups[j].size())
        update(static_cast<Symbol>(id / b_ / b_), i, j, false, 0, entry.second.minimum);
    }
  }
  void join(Symbol head, Symbol left, Symbol right, std::size_t i,
            std::size_t k, std::size_t j) {
    if (o_.max_dense_joins && stats_.joins >= o_.max_dense_joins)
      throw ResourceLimit("CERT-CFL dense join budget exceeded");
    ++stats_.joins;
    const Tile l = d_.tiles[d_.index(left, i, k)];
    const Tile r = d_.tiles[d_.index(right, k, j)];
    const auto middle = d_.groups[k].size();
    auto out = l.out ? r.out : 0;
    auto in = r.in ? l.in : 0;
    // Strict inequality is essential; subtraction avoids unsigned overflow.
    const bool overlap = l.out > middle - r.in;
    const bool wasFull = d_.full(head, i, j);
    if (overlap) { out = d_.groups[j].size(); in = d_.groups[i].size(); }
    // NEVER infer MAY from positive lower bounds: they may come from epsilon.
    update(head, i, j, l.may && r.may, out, in);
    if (overlap && !wasFull) {
      ++stats_.overlap_promotions;
      if (l.out < middle && r.in < middle)
        ++stats_.genuine_cardinality_promotions;
    }
  }
  const Prepared &p_;
  const Options &o_;
  Statistics &stats_;
  Data d_;
  std::size_t b_ = 0;
  std::deque<std::size_t> queue_;
};

Data sparseFallback(const Prepared &p, const Options &o, Statistics &stats) {
  std::unordered_set<Seed, SeedHash> facts;
  std::vector<Adjacency> out(p.symbols), in(p.symbols);
  std::deque<Seed> work;
  auto add = [&](Symbol a, Node u, Node v) {
    const Seed e{a, u, v};
    if (facts.find(e) != facts.end()) { ++stats.sparse_duplicate_attempts; return; }
    if (o.max_sparse_facts && facts.size() >= o.max_sparse_facts)
      throw ResourceLimit("CERT-CFL sparse fact budget exceeded");
    facts.insert(e); out[a][u].push_back(v); in[a][v].push_back(u); work.push_back(e);
    stats.peak_queue = std::max(stats.peak_queue, work.size());
  };
  for (const auto &e : p.seeds) add(e.symbol, e.source, e.target);
  for (std::size_t a = 0; a < p.symbols; ++a)
    if (p.nullable[a])
      for (Node u = 0; u < p.nodes; ++u) add(static_cast<Symbol>(a), u, u);
  while (!work.empty()) {
    const Seed selected = work.front(); work.pop_front(); ++stats.sparse_work_items;
    for (Symbol head : p.units[selected.symbol]) add(head, selected.source, selected.target);
    for (const auto &r : p.left[selected.symbol]) {
      const auto it = out[r.second].find(selected.target);
      if (it == out[r.second].end()) continue;
      // Map element references survive rehash. Index the vector afresh after
      // each insertion; do not retain iterators into a possibly growing vector.
      const auto &neighbors = it->second;
      const auto end = neighbors.size();
      for (std::size_t t = 0; t < end; ++t) {
        const Node target = neighbors[t]; ++stats.sparse_joins;
        add(r.first, selected.source, target);
      }
    }
    for (const auto &r : p.right[selected.symbol]) {
      const auto it = in[r.second].find(selected.source);
      if (it == in[r.second].end()) continue;
      const auto &neighbors = it->second;
      const auto end = neighbors.size();
      for (std::size_t t = 0; t < end; ++t) {
        const Node source = neighbors[t]; ++stats.sparse_joins;
        add(r.first, source, selected.target);
      }
    }
  }
  stats.used_sparse_fallback = true; stats.sparse_facts = facts.size(); stats.final_blocks = 0;
  Data d;
  d.nodes = p.nodes; d.symbols = p.symbols; d.nullable = p.nullable;
  d.sparse_symbol.assign(p.symbols, 1);
  // Reuse adjacency allocations rather than constructing a second index.
  d.successors = std::move(out); d.predecessors = std::move(in);
  for (std::size_t a = 0; a < p.symbols; ++a) if (p.nullable[a]) {
    for (auto &row : d.successors[a]) {
      auto &v = row.second; v.erase(std::remove(v.begin(), v.end(), row.first), v.end());
    }
    for (auto &row : d.predecessors[a]) {
      auto &v = row.second; v.erase(std::remove(v.begin(), v.end(), row.first), v.end());
    }
  }
  finish(d); d.stats = stats;
  return d;
}
bool requiredResolved(const Data &d, const Prepared &p) {
  for (Symbol a : p.observed) if (!d.exact[a]) return false;
  return true;
}
Data evaluate(const Prepared &p, const Options &o) {
  // Validate even when a budget would immediately trigger fallback.
  auto groups = initialPartition(p.nodes, o.initial_partition);
  Statistics stats;
  stats.explicit_seed_symbols = static_cast<std::size_t>(
      std::count(p.direct.begin(), p.direct.end(), std::uint8_t{1}));
  try {
    for (;;) {
      if (o.max_levels && stats.levels >= o.max_levels)
        throw ResourceLimit("CERT-CFL refinement-level budget exceeded");
      std::size_t tileCount;
      try { tileCount = checkedMul(checkedMul(p.symbols, groups.size()), groups.size()); }
      catch (const std::overflow_error &) { throw ResourceLimit("CERT-CFL tile dimensions overflow"); }
      if (o.max_tiles && tileCount > o.max_tiles)
        throw ResourceLimit("CERT-CFL tile budget exceeded");
      ++stats.levels; stats.final_blocks = groups.size();
      stats.peak_tiles = std::max(stats.peak_tiles, tileCount);
      auto d = DenseLevel(p, o, groups, stats, tileCount).run();
      if (requiredResolved(d, p)) { d.stats = stats; return d; }
      if (groups.size() == p.nodes)
        throw std::logic_error("CERT-CFL singleton completeness invariant failed");
      groups = refine(groups);
    }
  } catch (const ResourceLimit &) {
    if (o.on_limit == LimitAction::Throw) throw;
  }
  // A budget-triggered fallback is exact, not an approximation. Allocation
  // failure is deliberately NOT caught and retried with further allocation.
  return sparseFallback(p, o, stats);
}

const std::vector<Node> *lookup(const std::vector<Adjacency> &maps, Symbol a, Node u) {
  const auto it = maps[a].find(u);
  return it == maps[a].end() ? nullptr : &it->second;
}
} // namespace

std::size_t SeedHash::operator()(const Seed &e) const noexcept {
  return mix(mix(e.symbol, e.source), e.target);
}
struct Result::Impl { explicit Impl(Data value) : data(std::move(value)) {} Data data; };
Result::Result(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
Result::~Result() = default;
Result solve(const Problem &p, const Options &o) {
  const auto prepared = prepare(p, o);
  return Result(std::make_shared<Result::Impl>(evaluate(prepared, o)));
}
std::size_t Result::nodeCount() const { return impl_->data.nodes; }
std::size_t Result::symbolCount() const { return impl_->data.symbols; }
std::size_t Result::blockCount() const { return impl_->data.groups.size(); }
bool Result::resolved(Symbol a) const { impl_->data.requireSymbol(a); return impl_->data.exact[a] != 0; }
bool Result::nullable(Symbol a) const { impl_->data.requireSymbol(a); return impl_->data.nullable[a] != 0; }
const Statistics &Result::statistics() const { return impl_->data.stats; }
const Partition &Result::blocks() const { return impl_->data.groups; }
TileSummary Result::tile(Symbol a, std::size_t i, std::size_t j) const {
  const auto &d = impl_->data; d.requireSymbol(a);
  if (i >= d.groups.size() || j >= d.groups.size())
    throw std::out_of_range("CERT-CFL tile out of range (or sparse fallback)");
  const auto &t = d.tiles[d.index(a, i, j)]; return {t.may, t.out, t.in};
}
std::optional<bool> Result::answer(Symbol a, Node u, Node v) const {
  const auto &d = impl_->data; d.requireSymbol(a); d.requireNode(u); d.requireNode(v);
  if (d.nullable[a] && u == v) return true;
  if (d.sparse_symbol[a]) {
    const auto *row = lookup(d.successors, a, u);
    return row && std::binary_search(row->begin(), row->end(), v);
  }
  const auto i = d.owner[u], j = d.owner[v];
  const auto &t = d.tiles[d.index(a, i, j)];
  if (t.out == d.groups[j].size()) return true;
  if (!t.may) return false;
  return std::nullopt;
}
bool Result::contains(Symbol a, Node u, Node v) const {
  const auto value = answer(a, u, v);
  if (!value) throw std::logic_error("CERT-CFL contains() encountered UNKNOWN");
  return *value;
}
bool Result::visitSuccessors(Symbol a, Node u, const NodeVisitor &visitor) const {
  const auto &d = impl_->data; d.requireExact(a); d.requireNode(u);
  if (!visitor) throw std::invalid_argument("CERT-CFL empty node visitor");
  if (d.sparse_symbol[a]) {
    if (d.nullable[a] && !visitor(u)) return false;
    if (const auto *row = lookup(d.successors, a, u))
      for (Node v : *row) if (!visitor(v)) return false;
    return true;
  }
  const auto i = d.owner[u];
  for (std::size_t j = 0; j < d.groups.size(); ++j) {
    if (d.full(a, i, j)) {
      for (Node v : d.groups[j]) if (!visitor(v)) return false;
    } else if (d.nullable[a] && i == j && !visitor(u)) return false;
  }
  return true;
}
bool Result::visitPredecessors(Symbol a, Node v, const NodeVisitor &visitor) const {
  const auto &d = impl_->data; d.requireExact(a); d.requireNode(v);
  if (!visitor) throw std::invalid_argument("CERT-CFL empty node visitor");
  if (d.sparse_symbol[a]) {
    if (d.nullable[a] && !visitor(v)) return false;
    if (const auto *row = lookup(d.predecessors, a, v))
      for (Node u : *row) if (!visitor(u)) return false;
    return true;
  }
  const auto j = d.owner[v];
  for (std::size_t i = 0; i < d.groups.size(); ++i) {
    if (d.full(a, i, j)) {
      for (Node u : d.groups[i]) if (!visitor(u)) return false;
    } else if (d.nullable[a] && i == j && !visitor(v)) return false;
  }
  return true;
}
bool Result::visitEdges(Symbol a, const EdgeVisitor &visitor) const {
  const auto &d = impl_->data; d.requireExact(a);
  if (!visitor) throw std::invalid_argument("CERT-CFL empty edge visitor");
  // Sparse symbols can be visited in O(n*nullable + |E|), not O(n) for empty labels.
  if (d.sparse_symbol[a]) {
    if (d.nullable[a])
      for (Node u = 0; u < d.nodes; ++u) if (!visitor(Seed{a, u, u})) return false;
    for (const auto &row : d.successors[a])
      for (Node v : row.second) if (!visitor(Seed{a, row.first, v})) return false;
    return true;
  }
  for (std::size_t i = 0; i < d.groups.size(); ++i)
    for (std::size_t j = 0; j < d.groups.size(); ++j) {
      if (d.full(a, i, j)) {
        for (Node u : d.groups[i]) for (Node v : d.groups[j])
          if (!visitor(Seed{a, u, v})) return false;
      } else if (d.nullable[a] && i == j)
        for (Node u : d.groups[i]) if (!visitor(Seed{a, u, u})) return false;
    }
  return true;
}
bool Result::visitEdges(const EdgeVisitor &visitor) const {
  const auto &d = impl_->data;
  // Validate before any callback; do not partly enumerate and then fail.
  for (std::size_t a = 0; a < d.symbols; ++a) d.requireExact(static_cast<Symbol>(a));
  if (!visitor) throw std::invalid_argument("CERT-CFL empty edge visitor");
  for (std::size_t a = 0; a < d.symbols; ++a)
    if (!visitEdges(static_cast<Symbol>(a), visitor)) return false;
  return true;
}
std::size_t Result::edgeCount(Symbol a) const { impl_->data.requireExact(a); return impl_->data.counts[a]; }
std::size_t Result::edgeCount() const {
  std::size_t count = 0;
  for (std::size_t a = 0; a < symbolCount(); ++a)
    count = checkedAdd(count, edgeCount(static_cast<Symbol>(a)));
  return count;
}
std::size_t Result::countOffDiagonalUnion(std::vector<Symbol> symbols) const {
  const auto &d = impl_->data;
  for (Symbol a : symbols) d.requireExact(a);
  std::sort(symbols.begin(), symbols.end());
  symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
  std::vector<Symbol> dense, sparse;
  for (Symbol a : symbols) (d.sparse_symbol[a] ? sparse : dense).push_back(a);
  // Bulk-count the union of dense FULL rectangles once. Identities contribute
  // nothing because this query excludes the diagonal.
  std::vector<std::uint8_t> covered;
  std::size_t total = 0;
  const auto b = d.groups.size();
  if (!dense.empty()) {
    covered.assign(checkedMul(b, b), 0);
    for (std::size_t i = 0; i < b; ++i)
      for (std::size_t j = 0; j < b; ++j)
        for (Symbol a : dense) if (d.full(a, i, j)) {
          covered[i * b + j] = 1;
          auto count = checkedMul(d.groups[i].size(), d.groups[j].size());
          if (i == j) count -= d.groups[i].size();
          total = checkedAdd(total, count); break;
        }
  }
  // Deduplicate sparse remainders one source at a time. Do not materialize
  // the dense rectangles into the set, even in a hybrid query.
  if (!sparse.empty()) {
    std::unordered_set<Node> targets;
    for (Node u = 0; u < d.nodes; ++u) {
      targets.clear();
      for (Symbol a : sparse) if (const auto *row = lookup(d.successors, a, u))
        for (Node v : *row)
          if (u != v && (covered.empty() || !covered[d.owner[u] * b + d.owner[v]]))
            targets.insert(v);
      total = checkedAdd(total, targets.size());
    }
  }
  return total;
}
std::size_t Result::estimatedPayloadBytes() const {
  const auto &d = impl_->data;
  std::size_t bytes = sizeof(Impl);
  auto add = [&](std::size_t n, std::size_t size) { bytes = checkedAdd(bytes, checkedMul(n, size)); };
  add(d.tiles.capacity(), sizeof(Tile)); add(d.owner.capacity(), sizeof(std::size_t));
  add(d.groups.capacity(), sizeof(std::vector<Node>));
  for (const auto &g : d.groups) add(g.capacity(), sizeof(Node));
  add(d.nullable.capacity(), sizeof(std::uint8_t));
  add(d.exact.capacity(), sizeof(std::uint8_t));
  add(d.sparse_symbol.capacity(), sizeof(std::uint8_t));
  add(d.counts.capacity(), sizeof(std::size_t));
  for (const auto *maps : {&d.successors, &d.predecessors}) {
    add(maps->capacity(), sizeof(Adjacency));
    for (const auto &map : *maps) {
      add(map.bucket_count(), sizeof(void *));
      add(map.size(), sizeof(Adjacency::value_type));
      for (const auto &row : map) add(row.second.capacity(), sizeof(Node));
    }
  }
  return bytes;
}
} // namespace lotus::cfl::classical::engines::cert
