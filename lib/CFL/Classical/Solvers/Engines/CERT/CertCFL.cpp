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
std::uint64_t avalanche(std::uint64_t value) noexcept {
  // SplitMix64's finalizer prevents the dense, highly correlated symbol/node
  // IDs used by CFL relations from forming long unordered_set bucket chains.
  value ^= value >> 30U;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27U;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31U);
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
class PackedFactSet {
public:
  explicit PackedFactSet(std::size_t nodes, std::size_t symbols)
      : nodes_(nodes), packed_(canPack(nodes, symbols)) {}

  bool contains(const Seed &fact) const {
    if (!packed_)
      return wide_.find(fact) != wide_.end();
    if (slots_.empty())
      return false;
    const std::uint64_t key = pack(fact);
    std::size_t slot = static_cast<std::size_t>(avalanche(key)) &
                       (slots_.size() - 1U);
    while (slots_[slot] != EMPTY) {
      if (slots_[slot] == key)
        return true;
      slot = (slot + 1U) & (slots_.size() - 1U);
    }
    return false;
  }

  bool insert(const Seed &fact) {
    if (!packed_)
      return wide_.insert(fact).second;
    // Keep linear probes short while retaining a substantially smaller
    // footprint than one separately allocated unordered_set node per fact.
    if (slots_.empty() || size_ + 1U > slots_.size() - slots_.size() / 4U)
      grow();
    const std::uint64_t key = pack(fact);
    std::size_t slot = static_cast<std::size_t>(avalanche(key)) &
                       (slots_.size() - 1U);
    while (slots_[slot] != EMPTY) {
      if (slots_[slot] == key)
        return false;
      slot = (slot + 1U) & (slots_.size() - 1U);
    }
    slots_[slot] = key;
    ++size_;
    return true;
  }

  std::size_t size() const noexcept {
    return packed_ ? size_ : wide_.size();
  }

  void reserve(std::size_t count) {
    if (!packed_) {
      wide_.reserve(count);
      return;
    }
    std::size_t capacity = 16U;
    while (count > capacity - capacity / 4U) {
      if (capacity > std::numeric_limits<std::size_t>::max() / 2U)
        throw std::length_error("CERT-CFL sparse fact table overflow");
      capacity *= 2U;
    }
    if (capacity <= slots_.size()) return;
    std::vector<std::uint64_t> next(capacity, EMPTY);
    for (std::uint64_t key : slots_)
      if (key != EMPTY) place(next, key);
    slots_.swap(next);
  }

private:
  static constexpr std::uint64_t EMPTY = UINT64_MAX;

  static bool canPack(std::size_t nodes, std::size_t symbols) noexcept {
    const auto n = static_cast<std::uint64_t>(nodes);
    const auto s = static_cast<std::uint64_t>(symbols);
    if (n == 0 || s == 0)
      return true;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return n <= maximum / n && s <= maximum / (n * n);
  }

  std::uint64_t pack(const Seed &fact) const noexcept {
    const auto n = static_cast<std::uint64_t>(nodes_);
    return (static_cast<std::uint64_t>(fact.symbol) * n + fact.source) * n +
           fact.target;
  }

  static void place(std::vector<std::uint64_t> &slots,
                    std::uint64_t key) noexcept {
    std::size_t slot = static_cast<std::size_t>(avalanche(key)) &
                       (slots.size() - 1U);
    while (slots[slot] != EMPTY)
      slot = (slot + 1U) & (slots.size() - 1U);
    slots[slot] = key;
  }

  void grow() {
    if (!slots_.empty() &&
        slots_.size() > std::numeric_limits<std::size_t>::max() / 2U)
      throw std::length_error("CERT-CFL sparse fact table overflow");
    const std::size_t capacity = slots_.empty() ? 16U : slots_.size() * 2U;
    std::vector<std::uint64_t> next(capacity, EMPTY);
    for (std::uint64_t key : slots_)
      if (key != EMPTY)
        place(next, key);
    slots_.swap(next);
  }

  std::size_t nodes_ = 0;
  bool packed_ = false;
  std::size_t size_ = 0;
  std::vector<std::uint64_t> slots_;
  std::unordered_set<Seed, SeedHash> wide_;
};
enum class Bound : std::uint8_t { Zero, One, Majority, Full };
struct Tile {
  Bound out = Bound::Zero, in = Bound::Zero;
  bool may = false, queued = false;
};
static_assert(sizeof(Tile) == 4, "CERT-CFL tiles must remain compact");

std::size_t boundValue(Bound bound, std::size_t size) noexcept {
  switch (bound) {
  case Bound::Zero: return 0;
  case Bound::One: return 1;
  case Bound::Majority: return size / 2U + 1U;
  case Bound::Full: return size;
  }
  return 0;
}

Bound boundFor(std::size_t value, std::size_t size, bool majority) {
  const auto rounded = threshold(value, size, majority);
  if (rounded == 0) return Bound::Zero;
  if (rounded == size) return Bound::Full;
  if (majority && rounded == size / 2U + 1U) return Bound::Majority;
  return Bound::One;
}

bool active(const Tile &tile) noexcept {
  return tile.may || tile.out != Bound::Zero || tile.in != Bound::Zero;
}

class TileStore {
  static constexpr std::size_t EMPTY = std::numeric_limits<std::size_t>::max();
  struct Entry { std::size_t id = EMPTY; Tile tile; };

public:
  TileStore() = default;
  TileStore(std::size_t logical_size, bool dense) : dense_(dense) {
    if (dense_) dense_tiles_.resize(logical_size);
  }

  const Tile *find(std::size_t id) const noexcept {
    if (dense_)
      return active(dense_tiles_[id]) ? &dense_tiles_[id] : nullptr;
    if (entries_.empty()) return nullptr;
    std::size_t slot = static_cast<std::size_t>(avalanche(id)) &
                       (entries_.size() - 1U);
    while (entries_[slot].id != EMPTY) {
      if (entries_[slot].id == id) return &entries_[slot].tile;
      slot = (slot + 1U) & (entries_.size() - 1U);
    }
    return nullptr;
  }

  Tile *find(std::size_t id) noexcept {
    return const_cast<Tile *>(static_cast<const TileStore &>(*this).find(id));
  }

  Tile &getOrCreate(std::size_t id, bool &inserted) {
    if (dense_) {
      inserted = !active(dense_tiles_[id]);
      if (inserted) ++size_;
      return dense_tiles_[id];
    }
    if (entries_.empty() ||
        size_ + 1U > entries_.size() - entries_.size() / 4U)
      grow();
    std::size_t slot = static_cast<std::size_t>(avalanche(id)) &
                       (entries_.size() - 1U);
    while (entries_[slot].id != EMPTY) {
      if (entries_[slot].id == id) {
        inserted = false;
        return entries_[slot].tile;
      }
      slot = (slot + 1U) & (entries_.size() - 1U);
    }
    entries_[slot].id = id;
    ++size_;
    inserted = true;
    return entries_[slot].tile;
  }

  std::size_t activeSize() const noexcept { return size_; }
  std::size_t capacity() const noexcept {
    return dense_ ? dense_tiles_.capacity() : entries_.capacity();
  }
  std::size_t allocatedBytes() const noexcept {
    return dense_ ? dense_tiles_.capacity() * sizeof(Tile)
                  : entries_.capacity() * sizeof(Entry);
  }

  template <typename Visitor> void forEachActive(const Visitor &visitor) const {
    if (dense_) {
      for (std::size_t id = 0; id < dense_tiles_.size(); ++id)
        if (active(dense_tiles_[id])) visitor(id, dense_tiles_[id]);
      return;
    }
    for (const auto &entry : entries_)
      if (entry.id != EMPTY) visitor(entry.id, entry.tile);
  }

private:
  static void place(std::vector<Entry> &entries, Entry entry) noexcept {
    std::size_t slot = static_cast<std::size_t>(avalanche(entry.id)) &
                       (entries.size() - 1U);
    while (entries[slot].id != EMPTY)
      slot = (slot + 1U) & (entries.size() - 1U);
    entries[slot] = entry;
  }

  void grow() {
    if (!entries_.empty() &&
        entries_.size() > std::numeric_limits<std::size_t>::max() / 2U)
      throw std::length_error("CERT-CFL sparse tile table overflow");
    const std::size_t capacity = entries_.empty() ? 16U : entries_.size() * 2U;
    std::vector<Entry> next(capacity);
    for (const auto &entry : entries_)
      if (entry.id != EMPTY) place(next, entry);
    entries_.swap(next);
  }

  bool dense_ = false;
  std::size_t size_ = 0;
  std::vector<Tile> dense_tiles_;
  std::vector<Entry> entries_;
};

using Adjacency = std::unordered_map<Node, std::vector<Node>>;
using BlockAdjacency = std::unordered_map<std::size_t, std::vector<std::size_t>>;
struct Data {
  std::size_t nodes = 0, symbols = 0;
  Partition groups;
  std::vector<std::size_t> owner;
  TileStore tiles;
  BlockAdjacency tile_successors, tile_predecessors;
  std::vector<std::uint8_t> nullable, exact, sparse_symbol;
  // In these maps nullable diagonals are omitted and remain symbolic.
  std::vector<Adjacency> successors, predecessors;
  std::vector<std::size_t> counts;
  Statistics stats;
  std::size_t index(Symbol a, std::size_t i, std::size_t j) const {
    return (std::size_t{a} * groups.size() + i) * groups.size() + j;
  }
  bool full(Symbol a, std::size_t i, std::size_t j) const {
    const auto *tile = tiles.find(index(a, i, j));
    return tile && tile->out == Bound::Full;
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
  q.binary.erase(
      std::unique(q.binary.begin(), q.binary.end(),
                  [](const BinaryRule &a, const BinaryRule &b) {
                    return std::tie(a.lhs, a.first, a.second) ==
                           std::tie(b.lhs, b.first, b.second);
                  }),
      q.binary.end());

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
  q.unary.erase(
      std::unique(q.unary.begin(), q.unary.end(),
                  [](const UnaryRule &a, const UnaryRule &b) {
                    return a.lhs == b.lhs && a.rhs == b.rhs;
                  }),
      q.unary.end());
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
Partition singletonPartition(std::size_t nodes) {
  Partition groups(nodes);
  for (Node node = 0; node < nodes; ++node) groups[node].push_back(node);
  return groups;
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
    d.exact[a] = 1;
    d.counts[a] = d.nullable[a] ? d.nodes : 0;
  }
  const auto b = d.groups.size();
  d.tiles.forEachActive([&](std::size_t id, const Tile &tile) {
    const auto j = id % b, i = (id / b) % b;
    const auto a = static_cast<Symbol>(id / b / b);
    if (d.sparse_symbol[a]) return;
    if (tile.out == Bound::Full) {
      auto count = checkedMul(d.groups[i].size(), d.groups[j].size());
      if (d.nullable[a] && i == j) count -= d.groups[i].size();
      d.counts[a] = checkedAdd(d.counts[a], count);
    } else if (tile.may) {
      d.exact[a] = 0;
    }
  });
  for (std::size_t a = 0; a < d.symbols; ++a)
    if (!d.exact[a]) d.counts[a] = 0;
}

class DenseLevel {
public:
  DenseLevel(const Prepared &p, const Options &o, Partition groups,
             Statistics &stats, std::size_t tiles, bool dense_tiles)
      : p_(p), o_(o), stats_(stats) {
    d_.nodes = p.nodes; d_.symbols = p.symbols; d_.groups = std::move(groups);
    b_ = d_.groups.size(); d_.owner.resize(p.nodes);
    d_.tiles = TileStore(tiles, dense_tiles);
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
      auto *queued = d_.tiles.find(id);
      if (!queued) throw std::logic_error("CERT-CFL lost an active tile");
      queued->queued = false; ++stats_.queue_pops;
      const auto j = id % b_, i = (id / b_) % b_;
      const auto a = static_cast<Symbol>(id / b_ / b_);
      // Copy before applying rules: output and input may alias under recursion.
      const Tile selected = *queued;
      for (Symbol head : p_.units[a])
        update(head, i, j, selected.may,
               boundValue(selected.out, d_.groups[j].size()),
               boundValue(selected.in, d_.groups[i].size()));
      for (const auto &rule : p_.left[a]) {
        const auto found = d_.tile_successors.find(
            std::size_t{rule.second} * b_ + j);
        if (found == d_.tile_successors.end()) continue;
        const auto &targets = found->second;
        const auto end = targets.size();
        for (std::size_t index = 0; index < end; ++index) {
          const auto target = targets[index];
          join(rule.first, a, rule.second, i, j, target);
        }
      }
      for (const auto &rule : p_.right[a]) {
        const auto found = d_.tile_predecessors.find(
            std::size_t{rule.second} * b_ + i);
        if (found == d_.tile_predecessors.end()) continue;
        const auto &sources = found->second;
        const auto end = sources.size();
        for (std::size_t index = 0; index < end; ++index) {
          const auto source = sources[index];
          join(rule.first, rule.second, a, source, i, j);
        }
      }
    }
    finish(d_);
    return std::move(d_);
  }
private:
  void update(Symbol a, std::size_t i, std::size_t j, bool may,
              std::size_t out, std::size_t in) {
    const auto id = d_.index(a, i, j);
    const Tile *current = d_.tiles.find(id);
    const Tile old = current ? *current : Tile{};
    out = std::max(boundValue(old.out, d_.groups[j].size()),
                   threshold(out, d_.groups[j].size(), o_.majority_threshold));
    in = std::max(boundValue(old.in, d_.groups[i].size()),
                  threshold(in, d_.groups[i].size(), o_.majority_threshold));
    if (out == d_.groups[j].size() || in == d_.groups[i].size()) {
      out = d_.groups[j].size(); in = d_.groups[i].size();
    }
    may = may || old.may;
    const auto out_bound = boundFor(out, d_.groups[j].size(),
                                    o_.majority_threshold);
    const auto in_bound = boundFor(in, d_.groups[i].size(),
                                   o_.majority_threshold);
    if (old.may == may && old.out == out_bound && old.in == in_bound) return;
    if (!current && o_.max_tiles && d_.tiles.activeSize() >= o_.max_tiles)
      throw ResourceLimit("CERT-CFL active tile budget exceeded");
    bool inserted = false;
    Tile &t = d_.tiles.getOrCreate(id, inserted);
    if (inserted) {
      d_.tile_successors[std::size_t{a} * b_ + i].push_back(j);
      d_.tile_predecessors[std::size_t{a} * b_ + j].push_back(i);
      stats_.peak_tiles = std::max(stats_.peak_tiles, d_.tiles.activeSize());
    }
    t.may = may; t.out = out_bound; t.in = in_bound; ++stats_.updates;
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
    const auto *left_tile = d_.tiles.find(d_.index(left, i, k));
    const auto *right_tile = d_.tiles.find(d_.index(right, k, j));
    if (!left_tile || !right_tile) return;
    const Tile l = *left_tile, r = *right_tile;
    const auto middle = d_.groups[k].size();
    const auto left_out = boundValue(l.out, middle);
    const auto right_in = boundValue(r.in, middle);
    auto out = left_out ? boundValue(r.out, d_.groups[j].size()) : 0;
    auto in = right_in ? boundValue(l.in, d_.groups[i].size()) : 0;
    // Strict inequality is essential; subtraction avoids unsigned overflow.
    const bool overlap = left_out > middle - right_in;
    const bool wasFull = d_.full(head, i, j);
    if (overlap) { out = d_.groups[j].size(); in = d_.groups[i].size(); }
    // NEVER infer MAY from positive lower bounds: they may come from epsilon.
    update(head, i, j, l.may && r.may, out, in);
    if (overlap && !wasFull) {
      ++stats_.overlap_promotions;
      if (left_out < middle && right_in < middle)
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

Data concreteClosure(const Prepared &p, const Options &o, Statistics &stats,
                     Partition groups = {}, Data *base = nullptr) {
  PackedFactSet facts(p.nodes, p.symbols);
  std::vector<Adjacency> out = base ? std::move(base->successors)
                                    : std::vector<Adjacency>(p.symbols);
  std::vector<Adjacency> in = base ? std::move(base->predecessors)
                                   : std::vector<Adjacency>(p.symbols);
  std::deque<Seed> work;
  const std::size_t words = (p.nodes + 63U) / 64U;
  const std::size_t matrix_words = checkedMul(p.nodes, words);
  const std::size_t promotion_threshold = std::max(
      matrix_words, checkedMul(p.nodes, std::size_t{16}));
  constexpr std::size_t MAX_DENSE_MEMBERSHIPS = 8;
  std::vector<std::vector<std::uint64_t>> dense_membership(p.symbols);
  std::vector<std::size_t> symbol_facts(p.symbols, 0);
  std::size_t dense_memberships = 0;
  if (base) {
    std::size_t base_facts = 0;
    for (std::size_t a = 0; a < p.symbols; ++a)
      base_facts = checkedAdd(base_facts, base->counts[a]);
    facts.reserve(base_facts);
    for (std::size_t a = 0; a < p.symbols; ++a) {
      const auto symbol = static_cast<Symbol>(a);
      for (const auto &row : out[a])
        for (Node target : row.second) {
          if (!facts.insert(Seed{symbol, row.first, target}))
            throw std::logic_error("CERT-CFL base relation contains duplicates");
          ++symbol_facts[a];
        }
      if (p.nullable[a])
        for (Node node = 0; node < base->nodes; ++node) {
          if (!facts.insert(Seed{symbol, node, node}))
            throw std::logic_error("CERT-CFL base identity is not symbolic");
          ++symbol_facts[a];
        }
    }
    std::vector<Symbol> candidates(p.symbols);
    std::iota(candidates.begin(), candidates.end(), Symbol{0});
    std::partial_sort(
        candidates.begin(),
        candidates.begin() + std::min(candidates.size(), MAX_DENSE_MEMBERSHIPS),
        candidates.end(), [&](Symbol lhs, Symbol rhs) {
          return symbol_facts[lhs] > symbol_facts[rhs];
        });
    for (std::size_t position = 0;
         position < candidates.size() &&
         dense_memberships < MAX_DENSE_MEMBERSHIPS;
         ++position) {
      const Symbol a = candidates[position];
      if (symbol_facts[a] < promotion_threshold) break;
      auto &bits = dense_membership[a];
      bits.assign(matrix_words, 0);
      for (const auto &row : out[a])
        for (Node target : row.second)
          bits[row.first * words + target / 64U] |=
              UINT64_C(1) << (target % 64U);
      if (p.nullable[a])
        for (Node node = 0; node < base->nodes; ++node)
          bits[node * words + node / 64U] |= UINT64_C(1) << (node % 64U);
      ++dense_memberships;
    }
    stats.peak_tiles = facts.size();
  }
  auto denseContains = [&](Symbol a, Node u, Node v) {
    const auto &bits = dense_membership[a];
    return !bits.empty() &&
           (bits[u * words + v / 64U] &
            (UINT64_C(1) << (v % 64U))) != 0;
  };
  auto add = [&](Symbol a, Node u, Node v) {
    const Seed e{a, u, v};
    if (denseContains(a, u, v)) {
      ++stats.duplicate_attempts;
      return false;
    }
    if (o.max_tiles && facts.size() >= o.max_tiles) {
      if (facts.contains(e)) {
        ++stats.duplicate_attempts;
        return false;
      }
      throw ResourceLimit("CERT-CFL active tile budget exceeded");
    }
    if (!facts.insert(e)) {
      ++stats.duplicate_attempts;
      return false;
    }
    if (!dense_membership[a].empty())
      dense_membership[a][u * words + v / 64U] |=
          UINT64_C(1) << (v % 64U);
    out[a][u].push_back(v); in[a][v].push_back(u); work.push_back(e);
    ++symbol_facts[a];
    if (dense_membership[a].empty() && p.nodes != 0 &&
        symbol_facts[a] >= promotion_threshold &&
        dense_memberships < MAX_DENSE_MEMBERSHIPS) {
      auto &bits = dense_membership[a];
      bits.assign(matrix_words, 0);
      for (const auto &row : out[a])
        for (Node target : row.second)
          bits[row.first * words + target / 64U] |=
              UINT64_C(1) << (target % 64U);
      ++dense_memberships;
    }
    stats.peak_queue = std::max(stats.peak_queue, work.size());
    stats.peak_tiles = std::max(stats.peak_tiles, facts.size());
    return true;
  };
  for (const auto &e : p.seeds) add(e.symbol, e.source, e.target);
  if (!base) {
    for (std::size_t a = 0; a < p.symbols; ++a)
      if (p.nullable[a])
        for (Node u = 0; u < p.nodes; ++u) add(static_cast<Symbol>(a), u, u);
  } else {
    for (std::size_t a = 0; a < p.symbols; ++a)
      if (p.nullable[a])
        for (Node u = base->nodes; u < p.nodes; ++u)
          add(static_cast<Symbol>(a), u, u);
  }
  while (!work.empty()) {
    const Seed selected = work.front(); work.pop_front();
    ++stats.queue_pops;
    for (Symbol head : p.units[selected.symbol])
      add(head, selected.source, selected.target);
    for (const auto &r : p.left[selected.symbol]) {
      const auto it = out[r.second].find(selected.target);
      if (it == out[r.second].end()) continue;
      // Map element references survive rehash. Index the vector afresh after
      // each insertion; do not retain iterators into a possibly growing vector.
      const auto &neighbors = it->second;
      const auto end = neighbors.size();
      for (std::size_t t = 0; t < end; ++t) {
        const Node target = neighbors[t];
        ++stats.joins;
        add(r.first, selected.source, target);
      }
    }
    for (const auto &r : p.right[selected.symbol]) {
      const auto it = in[r.second].find(selected.source);
      if (it == in[r.second].end()) continue;
      const auto &neighbors = it->second;
      const auto end = neighbors.size();
      for (std::size_t t = 0; t < end; ++t) {
        const Node source = neighbors[t];
        ++stats.joins;
        add(r.first, source, selected.target);
      }
    }
  }
  stats.facts = facts.size();
  stats.final_blocks = groups.size();
  Data d;
  d.nodes = p.nodes; d.symbols = p.symbols; d.nullable = p.nullable;
  d.sparse_symbol.assign(p.symbols, 1);
  d.groups = std::move(groups);
  d.owner.resize(d.groups.empty() ? 0 : p.nodes);
  for (std::size_t block = 0; block < d.groups.size(); ++block)
    for (Node node : d.groups[block]) d.owner[node] = block;
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
  auto groups = initialPartition(p.nodes, o.initial_partition);
  Statistics stats;
  stats.explicit_seed_symbols = static_cast<std::size_t>(
      std::count(p.direct.begin(), p.direct.end(), std::uint8_t{1}));
  for (;;) {
    if (o.max_levels && stats.levels >= o.max_levels)
      throw ResourceLimit("CERT-CFL refinement-level budget exceeded");
    if (groups.size() == p.nodes) {
      // At singleton granularity every nonzero degree certificate is FULL.
      ++stats.levels;
      return concreteClosure(p, o, stats, std::move(groups));
    }
    std::size_t tileCount;
    try {
      tileCount = checkedMul(checkedMul(p.symbols, groups.size()),
                             groups.size());
    } catch (const std::overflow_error &) {
      throw ResourceLimit("CERT-CFL tile dimensions overflow");
    }
    ++stats.levels; stats.final_blocks = groups.size();
    constexpr std::size_t DENSE_TILE_THRESHOLD = 4U * 1024U * 1024U;
    const bool dense_tiles = tileCount <= DENSE_TILE_THRESHOLD;
    const auto promotions_before = stats.overlap_promotions;
    auto d = DenseLevel(p, o, groups, stats, tileCount, dense_tiles).run();
    if (requiredResolved(d, p)) { d.stats = stats; return d; }
    // With no promotion, intervening balanced levels only replay the same
    // derivations. Singleton refinement remains exact.
    if (groups.size() >= 64U &&
        stats.overlap_promotions == promotions_before)
      groups = singletonPartition(p.nodes);
    else
      groups = refine(groups);
  }
}

Data materializeConcrete(Data &old) {
  Data data;
  data.nodes = old.nodes;
  data.symbols = old.symbols;
  data.nullable = old.nullable;
  data.exact.assign(old.symbols, 1);
  data.sparse_symbol.assign(old.symbols, 1);
  data.counts = old.counts;
  data.groups = singletonPartition(old.nodes);
  data.owner.resize(old.nodes);
  std::iota(data.owner.begin(), data.owner.end(), std::size_t{0});
  data.successors.resize(old.symbols);
  data.predecessors.resize(old.symbols);
  const auto blocks = old.groups.size();
  for (std::size_t a = 0; a < old.symbols; ++a) {
    if (old.sparse_symbol[a]) {
      data.successors[a] = std::move(old.successors[a]);
      data.predecessors[a] = std::move(old.predecessors[a]);
      continue;
    }
    const auto symbol = static_cast<Symbol>(a);
    for (std::size_t source_block = 0; source_block < blocks;
         ++source_block) {
      const auto found = old.tile_successors.find(a * blocks + source_block);
      if (found == old.tile_successors.end()) continue;
      for (std::size_t target_block : found->second) {
        if (!old.full(symbol, source_block, target_block)) continue;
        for (Node source : old.groups[source_block])
          for (Node target : old.groups[target_block]) {
            if (old.nullable[a] && source == target) continue;
            data.successors[a][source].push_back(target);
            data.predecessors[a][target].push_back(source);
          }
      }
    }
  }
  return data;
}

const std::vector<Node> *lookup(const std::vector<Adjacency> &maps, Symbol a,
                                Node u) {
  const auto it = maps[a].find(u);
  return it == maps[a].end() ? nullptr : &it->second;
}
} // namespace

std::size_t SeedHash::operator()(const Seed &e) const noexcept {
  std::uint64_t hash = avalanche(static_cast<std::uint64_t>(e.symbol) +
                                 UINT64_C(0x9e3779b97f4a7c15));
  hash = avalanche(hash ^ avalanche(static_cast<std::uint64_t>(e.source)));
  hash = avalanche(hash ^ avalanche(static_cast<std::uint64_t>(e.target)));
  return static_cast<std::size_t>(hash);
}
struct Result::Impl {
  explicit Impl(Data value) : data(std::move(value)) {}
  Data data;
};
Result::Result(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Result::~Result() = default;
Result::Result(Result &&) noexcept = default;
Result &Result::operator=(Result &&) noexcept = default;
Result solve(const Problem &p, const Options &o) {
  const auto prepared = prepare(p, o);
  return Result(std::make_unique<Result::Impl>(evaluate(prepared, o)));
}
void Result::extend(const Problem &p, const Options &o) {
  const auto prepared = prepare(p, o);
  auto &old = impl_->data;
  if (old.nodes > prepared.nodes || old.symbols != prepared.symbols)
    throw std::invalid_argument("CERT-CFL incremental relation shape changed");
  if (!std::all_of(old.exact.begin(), old.exact.end(),
                   [](std::uint8_t value) { return value != 0; }))
    throw std::invalid_argument(
        "CERT-CFL cannot extend an unresolved result");
  const bool concrete = old.groups.size() == old.nodes &&
      std::all_of(old.sparse_symbol.begin(), old.sparse_symbol.end(),
                  [](std::uint8_t value) { return value != 0; });
  if (!concrete) old = materializeConcrete(old);
  const auto old_nodes = old.nodes;
  Statistics stats;
  stats.levels = 1;
  stats.final_blocks = prepared.nodes;
  stats.explicit_seed_symbols = static_cast<std::size_t>(
      std::count(prepared.direct.begin(), prepared.direct.end(),
                 std::uint8_t{1}));
  Partition groups = std::move(old.groups);
  groups.reserve(prepared.nodes);
  for (Node node = old_nodes; node < prepared.nodes; ++node)
    groups.push_back({node});
  auto data = concreteClosure(prepared, o, stats, std::move(groups), &old);
  data.stats = stats;
  impl_->data = std::move(data);
}
std::size_t Result::nodeCount() const { return impl_->data.nodes; }
std::size_t Result::symbolCount() const { return impl_->data.symbols; }
std::size_t Result::blockCount() const { return impl_->data.groups.size(); }
bool Result::resolved(Symbol a) const {
  impl_->data.requireSymbol(a);
  return impl_->data.exact[a] != 0;
}
bool Result::nullable(Symbol a) const {
  impl_->data.requireSymbol(a);
  return impl_->data.nullable[a] != 0;
}
const Statistics &Result::statistics() const { return impl_->data.stats; }
const Partition &Result::blocks() const { return impl_->data.groups; }
TileSummary Result::tile(Symbol a, std::size_t i, std::size_t j) const {
  const auto &d = impl_->data; d.requireSymbol(a);
  if (i >= d.groups.size() || j >= d.groups.size())
    throw std::out_of_range("CERT-CFL tile out of range");
  const auto *t = d.tiles.find(d.index(a, i, j));
  if (!t) {
    if (d.sparse_symbol[a] && d.groups[i].size() == 1U &&
        d.groups[j].size() == 1U) {
      const Node source = d.groups[i].front(), target = d.groups[j].front();
      const auto *row = lookup(d.successors, a, source);
      const bool explicit_fact =
          row && std::binary_search(row->begin(), row->end(), target);
      const bool present = explicit_fact || (d.nullable[a] && source == target);
      return {explicit_fact, present ? 1U : 0U, present ? 1U : 0U};
    }
    return {};
  }
  return {t->may, boundValue(t->out, d.groups[j].size()),
          boundValue(t->in, d.groups[i].size())};
}
std::optional<bool> Result::answer(Symbol a, Node u, Node v) const {
  const auto &d = impl_->data; d.requireSymbol(a); d.requireNode(u); d.requireNode(v);
  if (d.nullable[a] && u == v) return true;
  if (d.sparse_symbol[a]) {
    const auto *row = lookup(d.successors, a, u);
    return row && std::binary_search(row->begin(), row->end(), v);
  }
  const auto i = d.owner[u], j = d.owner[v];
  const auto *t = d.tiles.find(d.index(a, i, j));
  if (!t) return false;
  if (t->out == Bound::Full) return true;
  if (!t->may) return false;
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
  bool full_diagonal = false;
  const auto found = d.tile_successors.find(std::size_t{a} * d.groups.size() + i);
  if (found != d.tile_successors.end())
    for (std::size_t j : found->second) if (d.full(a, i, j)) {
      full_diagonal = full_diagonal || i == j;
      for (Node v : d.groups[j]) if (!visitor(v)) return false;
    }
  if (d.nullable[a] && !full_diagonal && !visitor(u)) return false;
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
  bool full_diagonal = false;
  const auto found = d.tile_predecessors.find(std::size_t{a} * d.groups.size() + j);
  if (found != d.tile_predecessors.end())
    for (std::size_t i : found->second) if (d.full(a, i, j)) {
      full_diagonal = full_diagonal || i == j;
      for (Node u : d.groups[i]) if (!visitor(u)) return false;
    }
  if (d.nullable[a] && !full_diagonal && !visitor(v)) return false;
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
  const auto b = d.groups.size();
  for (std::size_t i = 0; i < b; ++i) {
    bool full_diagonal = false;
    const auto found = d.tile_successors.find(std::size_t{a} * b + i);
    if (found != d.tile_successors.end())
      for (std::size_t j : found->second) if (d.full(a, i, j)) {
        full_diagonal = full_diagonal || i == j;
        for (Node u : d.groups[i]) for (Node v : d.groups[j])
          if (!visitor(Seed{a, u, v})) return false;
      }
    if (d.nullable[a] && !full_diagonal)
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
std::size_t Result::edgeCount(Symbol a) const {
  impl_->data.requireExact(a);
  return impl_->data.counts[a];
}
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
  // Count the union of FULL rectangles one source block at a time. This keeps
  // memory proportional to a row even when refinement reaches singletons.
  std::size_t total = 0;
  const auto b = d.groups.size();
  if (!dense.empty()) {
    std::unordered_set<std::size_t> target_blocks;
    for (std::size_t i = 0; i < b; ++i) {
      target_blocks.clear();
      for (Symbol a : dense) {
        const auto found = d.tile_successors.find(std::size_t{a} * b + i);
        if (found == d.tile_successors.end()) continue;
        for (std::size_t j : found->second)
          if (d.full(a, i, j)) target_blocks.insert(j);
      }
      for (std::size_t j : target_blocks) {
        auto count = checkedMul(d.groups[i].size(), d.groups[j].size());
        if (i == j) count -= d.groups[i].size();
        total = checkedAdd(total, count);
      }
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
          if (u != v) {
            bool covered = false;
            for (Symbol dense_symbol : dense)
              if (d.full(dense_symbol, d.owner[u], d.owner[v])) {
                covered = true;
                break;
              }
            if (!covered) targets.insert(v);
          }
      total = checkedAdd(total, targets.size());
    }
  }
  return total;
}
std::size_t Result::estimatedPayloadBytes() const {
  const auto &d = impl_->data;
  std::size_t bytes = sizeof(Impl);
  auto add = [&](std::size_t n, std::size_t size) {
    bytes = checkedAdd(bytes, checkedMul(n, size));
  };
  bytes = checkedAdd(bytes, d.tiles.allocatedBytes());
  add(d.owner.capacity(), sizeof(std::size_t));
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
  for (const auto *map : {&d.tile_successors, &d.tile_predecessors}) {
    add(map->bucket_count(), sizeof(void *));
    add(map->size(), sizeof(BlockAdjacency::value_type));
    for (const auto &row : *map)
      add(row.second.capacity(), sizeof(std::size_t));
  }
  return bytes;
}
} // namespace lotus::cfl::classical::engines::cert
