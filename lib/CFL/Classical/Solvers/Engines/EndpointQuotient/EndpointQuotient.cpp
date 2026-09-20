#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus {
namespace cfl {
namespace endpoint {
namespace {

using Clock = std::chrono::steady_clock;
using Lists = std::vector<std::vector<Id>>;
using ClassId = std::uint32_t;
using ClassLists = std::vector<std::vector<ClassId>>;

static double milliseconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

static Count product(Id a, Id b) {
  static_assert(sizeof(Id) <= sizeof(Count), "IDs must fit in counters");
  if (b && a > std::numeric_limits<Count>::max() / b)
    throw std::overflow_error("endpoint quotient fact count overflow");
  return static_cast<Count>(a) * static_cast<Count>(b);
}

static void addCount(Count &a, Count b) {
  if (a > std::numeric_limits<Count>::max() - b)
    throw std::overflow_error("endpoint quotient fact count overflow");
  a += b;
}

template <class T> static void sortUnique(std::vector<T> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

static bool edgeLess(const Edge &lhs, const Edge &rhs) {
  return std::tie(lhs.symbol, lhs.source, lhs.target) <
         std::tie(rhs.symbol, rhs.source, rhs.target);
}

static bool edgeEqual(const Edge &lhs, const Edge &rhs) {
  return lhs.symbol == rhs.symbol && lhs.source == rhs.source &&
         lhs.target == rhs.target;
}

static std::vector<Edge> canonicalEdges(std::vector<Edge> edges) {
  std::sort(edges.begin(), edges.end(), edgeLess);
  edges.erase(std::unique(edges.begin(), edges.end(), edgeEqual), edges.end());
  return edges;
}

static bool sameRules(const std::vector<Rule> &lhs,
                      const std::vector<Rule> &rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (Id i = 0; i < lhs.size(); ++i)
    if (lhs[i].kind != rhs[i].kind || lhs[i].lhs != rhs[i].lhs ||
        lhs[i].left != rhs[i].left || lhs[i].right != rhs[i].right)
      return false;
  return true;
}

struct VectorHash {
  std::size_t operator()(const std::vector<Id> &v) const noexcept {
    std::size_t h = v.size();
    for (Id x : v)
      h ^= std::hash<Id>{}(x) + std::size_t(0x9e3779b9U) + (h << 6) + (h >> 2);
    return h;
  }
};

struct PairHash {
  std::size_t operator()(const std::pair<Id, Id> &pair) const noexcept {
    std::size_t hash = std::hash<Id>{}(pair.first);
    hash ^= std::hash<Id>{}(pair.second) + std::size_t(0x9e3779b9U) +
            (hash << 6) + (hash >> 2);
    return hash;
  }
};

static std::vector<Id> intern(const Lists &signatures) {
  std::unordered_map<std::vector<Id>, Id, VectorHash> table;
  table.reserve(signatures.size());
  std::vector<Id> result;
  result.reserve(signatures.size());
  for (const auto &signature : signatures) {
    const auto found = table.find(signature);
    if (found != table.end()) {
      result.push_back(found->second);
    } else {
      const Id id = table.size();
      table.emplace(signature, id);
      result.push_back(id);
    }
  }
  return result;
}

struct Partition {
  Id id = 0;
  std::vector<Id> class_of;
  Lists members;

  static Partition fromSignatures(const Lists &signatures) {
    Partition p;
    p.class_of = intern(signatures);
    for (Id v = 0; v < p.class_of.size(); ++v) {
      Id c = p.class_of[v];
      if (c == p.members.size())
        p.members.emplace_back();
      p.members[c].push_back(v);
    }
    return p;
  }

  static Partition singleton(Id n) {
    Partition p;
    p.class_of.resize(n);
    std::iota(p.class_of.begin(), p.class_of.end(), Id(0));
    p.members.resize(n);
    for (Id v = 0; v < n; ++v)
      p.members[v].push_back(v);
    return p;
  }
};

// Each fine class belongs to exactly one coarse class. The grammar dependency
// closure guarantees this refinement; checking all members in debug builds
// catches mistakes that representative-only quotient constructions can hide.
static ClassLists lift(const Partition &fine, const Partition &coarse) {
  ClassLists result(coarse.members.size());
  for (Id f = 0; f < fine.members.size(); ++f) {
    Id c = coarse.class_of[fine.members[f].front()];
#ifndef NDEBUG
    for (Id v : fine.members[f])
      assert(coarse.class_of[v] == c);
#endif
    result[c].push_back(static_cast<ClassId>(f));
  }
  return result;
}

static Lists dependencyClosure(const Lists &dependencies,
                               const std::vector<bool> &observed) {
  Lists closure(dependencies.size());
  Lists reverse(dependencies.size());
  for (Id a = 0; a < dependencies.size(); ++a)
    for (Id b : dependencies[a])
      reverse[b].push_back(a);
  for (auto &uses : reverse)
    sortUnique(uses);
  // Only labels with input edges can distinguish endpoints. Traverse their
  // reverse dependencies, retaining an O(symbols * observed labels) closure
  // instead of all reachable grammar symbols (including constant signatures).
  for (Id label = 0; label < dependencies.size(); ++label) {
    if (!observed[label])
      continue;
    std::vector<bool> seen(dependencies.size(), false);
    std::vector<Id> stack{label};
    seen[label] = true;
    while (!stack.empty()) {
      Id b = stack.back();
      stack.pop_back();
      closure[b].push_back(label);
      for (Id c : reverse[b]) {
        if (!seen[c]) {
          seen[c] = true;
          stack.push_back(c);
        }
      }
    }
  }
  return closure;
}

// Sparse relations do not allocate a hash table per row. Denser quotients
// promote to a bounded bitmap, making duplicate publication a bit test.
class CellSet {
  struct Row {
    Count size = 0;
    llvm::SparseBitVector<> sparse;
    std::vector<std::uint64_t> dense;

    void promote(Id columns) {
      dense.assign((columns + 63) / 64, 0);
      for (unsigned column : sparse)
        dense[column / 64] |= std::uint64_t(1) << (column % 64);
      sparse = llvm::SparseBitVector<>();
    }

    bool insert(Id column, Id columns) {
      if (!dense.empty()) {
        auto &word = dense[column / 64];
        const auto mask = std::uint64_t(1) << (column % 64);
        const bool added = !(word & mask);
        word |= mask;
        size += added ? 1 : 0;
        return added;
      }
      const bool added = sparse.test_and_set(column);
      size += added ? 1 : 0;
      constexpr Id MAX_DENSE_ROW_WORDS = 256;
      if (added && columns && (columns + 63) / 64 <= MAX_DENSE_ROW_WORDS &&
          size >= (columns + 127) / 128)
        promote(columns);
      return added;
    }

    bool contains(Id column) const {
      return dense.empty() ? sparse.test(column)
                           : (dense[column / 64] &
                              (std::uint64_t(1) << (column % 64))) != 0;
    }

    template <class Visitor>
    Count insertMany(const std::vector<ClassId> &columns, Id total_columns,
                     Visitor visitor) {
      if (columns.size() < 32) {
        Count inserted = 0;
        for (Id column : columns)
          if (insert(column, total_columns)) {
            visitor(column);
            ++inserted;
          }
        return inserted;
      }
      if (!dense.empty()) {
        Count inserted = 0;
        for (Id column : columns) {
          auto &word = dense[column / 64];
          const auto mask = std::uint64_t(1) << (column % 64);
          if (word & mask)
            continue;
          word |= mask;
          visitor(column);
          ++inserted;
        }
        size += inserted;
        return inserted;
      }

      llvm::SparseBitVector<> candidates;
      for (Id column : columns)
        candidates.set(column);
      llvm::SparseBitVector<> delta;
      delta.intersectWithComplement(candidates, sparse);
      const Count inserted = delta.count();
      sparse |= delta;
      size += inserted;
      for (unsigned column : delta)
        visitor(column);
      constexpr Id MAX_DENSE_ROW_WORDS = 256;
      if (inserted && total_columns &&
          (total_columns + 63) / 64 <= MAX_DENSE_ROW_WORDS &&
          size >= (total_columns + 127) / 128)
        promote(total_columns);
      return inserted;
    }

    template <class Visitor> void forEach(Visitor visitor) const {
      if (dense.empty()) {
        for (unsigned column : sparse)
          visitor(column);
        return;
      }
      for (Id word = 0; word < dense.size(); ++word) {
        auto value = dense[word];
        while (value) {
          visitor(word * 64 + __builtin_ctzll(value));
          value &= value - 1;
        }
      }
    }

    std::size_t payloadBytes() const {
      return dense.capacity() * sizeof(std::uint64_t) +
             (dense.empty() ? size * sizeof(unsigned) : 0);
    }
  };

  Id columns_ = 0;
  Id area_ = 0;
  Count sparse_size_ = 0;
  std::vector<std::uint64_t> bits_;
  std::vector<Row> sparse_rows_;

  void promote() {
    bits_.assign((area_ + 63) / 64, 0);
    for (Id row = 0; row < sparse_rows_.size(); ++row)
      sparse_rows_[row].forEach([&](Id column) {
        const Id bit = row * columns_ + column;
        bits_[bit / 64] |= std::uint64_t(1) << (bit % 64);
      });
    decltype(sparse_rows_)().swap(sparse_rows_);
  }

public:
  void reset(Id rows, Id columns) {
    columns_ = columns;
    area_ = 0;
    sparse_size_ = 0;
    bits_.clear();
    sparse_rows_.clear();
    constexpr Id MAX_BITS = 8 * 1024 * 1024;
    if (columns && rows <= MAX_BITS / columns)
      area_ = rows * columns;
    if (area_ && area_ <= 4096)
      promote();
    else
      sparse_rows_.resize(rows);
  }

  bool insert(Id row, Id column) {
    if (!bits_.empty()) {
      const Id bit = row * columns_ + column;
      auto &word = bits_[bit / 64];
      const auto mask = std::uint64_t(1) << (bit % 64);
      const bool added = !(word & mask);
      word |= mask;
      return added;
    }
    const bool added = sparse_rows_[row].insert(column, columns_);
    sparse_size_ += added ? 1 : 0;
    if (added && area_ && sparse_size_ >= (area_ + 127) / 128)
      promote();
    return added;
  }

  bool contains(Id row, Id column) const {
    if (bits_.empty())
      return sparse_rows_[row].contains(column);
    const Id bit = row * columns_ + column;
    return bits_[bit / 64] & (std::uint64_t(1) << (bit % 64));
  }

  template <class Visitor>
  Count insertMany(Id row, const std::vector<ClassId> &columns,
                   Visitor visitor) {
    if (!bits_.empty()) {
      Count inserted = 0;
      for (Id column : columns) {
        const Id bit = row * columns_ + column;
        auto &word = bits_[bit / 64];
        const auto mask = std::uint64_t(1) << (bit % 64);
        if (word & mask)
          continue;
        word |= mask;
        visitor(column);
        ++inserted;
      }
      return inserted;
    }
    const Count inserted =
        sparse_rows_[row].insertMany(columns, columns_, visitor);
    sparse_size_ += inserted;
    if (inserted && area_ && sparse_size_ >= (area_ + 127) / 128)
      promote();
    return inserted;
  }

  bool isDense() const { return !bits_.empty(); }

  std::uint64_t rowWord(Id row, Id word) const {
    assert(isDense());
    const Id offset = row * columns_ + word * 64;
    const Id shift = offset % 64;
    auto value = bits_[offset / 64] >> shift;
    if (shift && offset / 64 + 1 < bits_.size())
      value |= bits_[offset / 64 + 1] << (64 - shift);
    const Id remaining = columns_ - word * 64;
    if (remaining < 64)
      value &= (std::uint64_t(1) << remaining) - 1;
    return value;
  }

  template <class Visitor> void forEach(Visitor visitor) const {
    if (!isDense()) {
      for (Id row = 0; row < sparse_rows_.size(); ++row)
        sparse_rows_[row].forEach([&](Id column) { visitor(row, column); });
      return;
    }
    for (Id w = 0; w < bits_.size(); ++w) {
      auto bits = bits_[w];
      while (bits) {
        const Id bit = w * 64 + __builtin_ctzll(bits);
        visitor(bit / columns_, bit % columns_);
        bits &= bits - 1;
      }
    }
  }

  std::size_t payloadBytes() const {
    std::size_t bytes = bits_.capacity() * sizeof(std::uint64_t) +
                        sparse_rows_.capacity() * sizeof(Row);
    for (const Row &row : sparse_rows_)
      bytes += row.payloadBytes();
    return bytes;
  }
};

// Temporary, word-aligned indexes for batched joins. Unlike known cells,
// active cells contain only popped events, preserving once-per-pair joins.
struct JoinRows {
  Id out_words;
  Id in_words;
  std::vector<std::uint64_t> out;
  std::vector<std::uint64_t> in;
  std::vector<std::uint64_t> known_in;

  void know(Id row, Id column) {
    known_in[column * in_words + row / 64] |= std::uint64_t(1) << (row % 64);
  }

  void activate(Id row, Id column) {
    out[row * out_words + column / 64] |= std::uint64_t(1) << (column % 64);
    in[column * in_words + row / 64] |= std::uint64_t(1) << (row % 64);
  }
};

struct Relation {
  CellSet known;
  ClassLists active_out;
  ClassLists active_in;
  ClassLists pending_out;
  std::vector<bool> queued_rows;
  std::unique_ptr<JoinRows> join_rows;
  bool tried_join_rows = false;
};

struct UnaryPlan {
  Id lhs;
  Id child;
  std::shared_ptr<const ClassLists> rows;
  std::shared_ptr<const ClassLists> columns;
};

struct Bridge {
  ClassLists to_right;
  ClassLists to_left;
  Count pairs = 0;
};

struct BinaryPlan {
  Id lhs;
  Id left;
  Id right;
  std::shared_ptr<const ClassLists> rows;
  std::shared_ptr<const ClassLists> columns;
  // An interface edge (j,k) means Q_left[j] intersects P_right[k].
  // Numeric class ID equality has NO semantic meaning across partitions.
  std::shared_ptr<const Bridge> bridge;
  // Only nontrivial refinement products need a separate output cache.
  CellSet expanded;
  bool expanded_ready = false;
};

struct Fact {
  Id symbol;
  Id row;
  Id column;
};

struct DeltaRow {
  Id symbol;
  Id row;
};

struct FactorView {
  Id symbol = 0;
  Id source_origin = 0;
  Id target_origin = 0;
  std::shared_ptr<const Partition> source;
  std::shared_ptr<const Partition> target;
  std::shared_ptr<const ClassLists> global_rows;
  std::shared_ptr<const ClassLists> global_columns;
  CellSet known;
  ClassLists active_out;
  ClassLists active_in;
  ClassLists pending_out;
  std::vector<bool> queued_rows;
};

struct FactorUnaryPlan {
  Id lhs_view = 0;
  Id child_view = 0;
};

struct FactorBinaryPlan {
  Id lhs_view = 0;
  Id left_view = 0;
  Id right_view = 0;
  std::shared_ptr<const Bridge> bridge;
};

struct FactorDeltaRow {
  Id view = 0;
  Id row = 0;
};

} // namespace

void Problem::validate() const {
  if (nodes > std::numeric_limits<ClassId>::max())
    throw std::invalid_argument(
        "endpoint quotient node count exceeds 32-bit class IDs");
  for (const Edge &e : edges)
    if (e.source >= nodes || e.target >= nodes || e.symbol >= symbols)
      throw std::invalid_argument("endpoint quotient edge ID out of range");
  for (const Rule &r : rules) {
    if (r.lhs >= symbols)
      throw std::invalid_argument("endpoint quotient rule LHS out of range");
    switch (r.kind) {
    case Rule::Kind::Epsilon:
      break;
    case Rule::Kind::Unary:
      if (r.left >= symbols)
        throw std::invalid_argument("endpoint quotient unary RHS out of range");
      break;
    case Rule::Kind::Binary:
      if (r.left >= symbols || r.right >= symbols)
        throw std::invalid_argument(
            "endpoint quotient binary RHS out of range");
      break;
    default:
      throw std::invalid_argument("endpoint quotient invalid rule kind");
    }
  }
}

struct Solver::Impl {
  Problem problem;
  Options options;
  std::vector<Rule> grammar_rules;
  std::vector<Edge> seed_edges;
  bool solved = false;
  bool started = false;
  Statistics stats;
  std::vector<bool> nullable;
  std::vector<std::shared_ptr<const Partition>> sources;
  std::vector<std::shared_ptr<const Partition>> targets;
  std::vector<Relation> relations;
  std::vector<UnaryPlan> units;
  std::vector<BinaryPlan> binaries;
  Lists unit_uses;
  Lists left_uses;
  Lists right_uses;
  std::deque<DeltaRow> worklist;
  Count pending_cells = 0;
  const Impl *previous = nullptr;
  Id join_index_words = 0;
  std::vector<ClassId> bulk_values;
  std::vector<std::uint32_t> bulk_marks;
  std::uint32_t bulk_epoch = 0;
  std::vector<FactorView> factor_views;
  Lists symbol_factor_views;
  std::map<std::tuple<Id, Id, Id>, Id> factor_view_ids;
  std::vector<FactorUnaryPlan> factor_units;
  std::vector<FactorBinaryPlan> factor_binaries;
  Lists factor_unit_uses;
  Lists factor_left_uses;
  Lists factor_right_uses;
  std::deque<FactorDeltaRow> factor_worklist;
  Count factor_pending_cells = 0;

  Impl(Problem p, Options o, const Impl *old = nullptr)
      : problem(std::move(p)), options(o), grammar_rules(problem.rules),
        seed_edges(canonicalEdges(problem.edges)), previous(old) {
    problem.validate();
    switch (options.partitions) {
    case PartitionMode::Grammar:
    case PartitionMode::Global:
    case PartitionMode::Singleton:
      break;
    default:
      throw std::invalid_argument("endpoint quotient invalid partition mode");
    }
    if (previous &&
        (!previous->solved || previous->problem.symbols != problem.symbols ||
         previous->problem.nodes > problem.nodes ||
         previous->options.partitions != options.partitions ||
         previous->options.factorized != options.factorized ||
         !sameRules(previous->grammar_rules, grammar_rules) ||
         !std::includes(seed_edges.begin(), seed_edges.end(),
                        previous->seed_edges.begin(),
                        previous->seed_edges.end(), edgeLess)))
      throw std::invalid_argument(
          "endpoint quotient incremental snapshot is incompatible");
  }

  void requireSolved() const {
    if (!solved)
      throw std::logic_error(
          "endpoint quotient queried before solve completed");
  }

  void requireSymbol(Id a) const {
    if (a >= problem.symbols)
      throw std::out_of_range("endpoint quotient query symbol out of range");
  }

  void computeNullable() {
    nullable.assign(problem.symbols, false);
    Lists uses(problem.symbols);
    std::vector<Id> remaining(problem.rules.size());
    std::vector<Id> pending;
    auto mark = [&](Id symbol) {
      if (!nullable[symbol]) {
        nullable[symbol] = true;
        pending.push_back(symbol);
        ++stats.nullable_symbols;
      }
    };
    for (Id i = 0; i < problem.rules.size(); ++i) {
      const Rule &r = problem.rules[i];
      if (r.kind == Rule::Kind::Epsilon) {
        mark(r.lhs);
      } else {
        remaining[i] = r.kind == Rule::Kind::Unary ? 1 : 2;
        uses[r.left].push_back(i);
        if (r.kind == Rule::Kind::Binary)
          uses[r.right].push_back(i);
      }
    }
    for (Id i = 0; i < pending.size(); ++i)
      for (Id rule : uses[pending[i]])
        if (--remaining[rule] == 0)
          mark(problem.rules[rule].lhs);
  }

  void buildPartitions(const Lists &first, const Lists &last) {
    const Id k = problem.symbols, n = problem.nodes;
    sources.resize(k);
    targets.resize(k);
    // Canonical class numbering lets equal partitions share both membership
    // storage and all subsequent lift/bridge plans.
    std::unordered_map<std::vector<Id>, std::shared_ptr<const Partition>,
                       VectorHash>
        partitions;
    auto retain = [&](Partition p) {
      const auto found = partitions.find(p.class_of);
      if (found != partitions.end())
        return found->second;
      p.id = stats.partitions_built++;
      auto result = std::make_shared<const Partition>(std::move(p));
      partitions.emplace(result->class_of, result);
      return result;
    };
    if (options.partitions == PartitionMode::Singleton) {
      const auto p = retain(Partition::singleton(n));
      std::fill(sources.begin(), sources.end(), p);
      std::fill(targets.begin(), targets.end(), p);
      return;
    }

    std::vector<std::vector<Edge>> seeds(k);
    for (const Edge &e : problem.edges)
      seeds[e.symbol].push_back(e);
    Lists atomic_out(k), atomic_in(k);
    std::vector<Id> observed;
    for (Id a = 0; a < k; ++a) {
      // A symbol with no axioms contributes only a constant signature. Its
      // dependencies still participate in FIRST/LAST before this projection.
      if (seeds[a].empty())
        continue;
      observed.push_back(a);
      Lists out(n), in(n);
      for (const Edge &e : seeds[a]) {
        out[e.source].push_back(e.target);
        in[e.target].push_back(e.source);
      }
      for (auto &row : out)
        sortUnique(row);
      for (auto &column : in)
        sortUnique(column);
      atomic_out[a] = intern(out);
      atomic_in[a] = intern(in);
    }

    auto build = [&](const Lists &closure, const Lists &atomic,
                     std::vector<std::shared_ptr<const Partition>> &result) {
      std::unordered_map<std::vector<Id>, std::shared_ptr<const Partition>,
                         VectorHash>
          cache;
      for (Id a = 0; a < k; ++a) {
        std::vector<Id> labels;
        if (options.partitions == PartitionMode::Global) {
          labels = observed;
        } else {
          for (Id b : closure[a])
            if (!seeds[b].empty())
              labels.push_back(b);
        }
        const auto found = cache.find(labels);
        if (found != cache.end()) {
          result[a] = found->second;
          continue;
        }
        Lists signatures(n);
        for (Id v = 0; v < n; ++v) {
          signatures[v].reserve(labels.size());
          for (Id b : labels)
            signatures[v].push_back(atomic[b][v]);
        }
        result[a] = retain(Partition::fromSignatures(signatures));
        cache.emplace(std::move(labels), result[a]);
      }
    };
    build(first, atomic_out, sources);
    build(last, atomic_in, targets);
  }

  void refinePreviousPartitions() {
    if (!previous)
      return;
    const Id old_nodes = previous->problem.nodes;
    std::unordered_map<std::vector<Id>, std::shared_ptr<const Partition>,
                       VectorHash>
        partitions;
    std::map<std::pair<Id, Id>, std::shared_ptr<const Partition>> refinements;
    auto refine = [&](const std::shared_ptr<const Partition> &fresh,
                      const std::shared_ptr<const Partition> &old) {
      const std::pair<Id, Id> refinement_key{fresh->id, old->id};
      const auto refined = refinements.find(refinement_key);
      if (refined != refinements.end())
        return refined->second;

      Partition partition;
      partition.class_of.reserve(problem.nodes);
      std::unordered_map<std::pair<Id, Id>, Id, PairHash> classes;
      classes.reserve(problem.nodes);
      const Id new_node_class = old->members.size();
      for (Id v = 0; v < problem.nodes; ++v) {
        const std::pair<Id, Id> key{fresh->class_of[v], v < old_nodes
                                                            ? old->class_of[v]
                                                            : new_node_class};
        const Id next = classes.size();
        const auto [it, inserted] = classes.emplace(key, next);
        if (inserted)
          partition.members.emplace_back();
        partition.class_of.push_back(it->second);
        partition.members[it->second].push_back(v);
      }
      const auto found = partitions.find(partition.class_of);
      std::shared_ptr<const Partition> result;
      if (found != partitions.end()) {
        result = found->second;
      } else {
        partition.id = stats.partitions_built++;
        result = std::make_shared<const Partition>(std::move(partition));
        partitions.emplace(result->class_of, result);
      }
      refinements.emplace(refinement_key, result);
      return result;
    };
    for (Id a = 0; a < problem.symbols; ++a) {
      sources[a] = refine(sources[a], previous->sources[a]);
      targets[a] = refine(targets[a], previous->targets[a]);
    }
  }

  void migratePreviousClosure() {
    if (!previous)
      return;
    const Id old_nodes = previous->problem.nodes;
    std::map<std::pair<Id, Id>, std::shared_ptr<const ClassLists>> child_maps;
    auto getChildren = [&](const std::shared_ptr<const Partition> &fine,
                           const std::shared_ptr<const Partition> &coarse) {
      const std::pair<Id, Id> key{fine->id, coarse->id};
      const auto found = child_maps.find(key);
      if (found != child_maps.end())
        return found->second;
      auto children = std::make_shared<ClassLists>(coarse->members.size());
      for (Id v = 0; v < old_nodes; ++v)
        (*children)[coarse->class_of[v]].push_back(
            static_cast<ClassId>(fine->class_of[v]));
      for (auto &classes : *children)
        sortUnique(classes);
      child_maps.emplace(key, children);
      return std::shared_ptr<const ClassLists>(std::move(children));
    };
    for (Id a = 0; a < problem.symbols; ++a) {
      if (sources[a]->class_of == previous->sources[a]->class_of &&
          targets[a]->class_of == previous->targets[a]->class_of) {
        relations[a].known = previous->relations[a].known;
        relations[a].active_out = previous->relations[a].active_out;
        relations[a].active_in = previous->relations[a].active_in;
        for (const auto &row : relations[a].active_out)
          stats.cells += row.size();
        continue;
      }
      const auto source_children =
          getChildren(sources[a], previous->sources[a]);
      const auto target_children =
          getChildren(targets[a], previous->targets[a]);

      for (Id old_row = 0; old_row < previous->relations[a].active_out.size();
           ++old_row) {
        for (Id old_column : previous->relations[a].active_out[old_row]) {
          for (Id row : (*source_children)[old_row])
            for (Id column : (*target_children)[old_column]) {
              if (!relations[a].known.insert(row, column))
                continue;
              relations[a].active_out[row].push_back(column);
              relations[a].active_in[column].push_back(row);
              ++stats.cells;
            }
        }
      }
    }
  }

  Id addFactorView(Id symbol, Id source_origin, Id target_origin) {
    const auto key = std::make_tuple(symbol, source_origin, target_origin);
    const auto found = factor_view_ids.find(key);
    if (found != factor_view_ids.end())
      return found->second;
    const Id id = factor_views.size();
    FactorView view;
    view.symbol = symbol;
    view.source_origin = source_origin;
    view.target_origin = target_origin;
    view.source = sources[source_origin];
    view.target = targets[target_origin];
    view.known.reset(view.source->members.size(), view.target->members.size());
    view.active_out.resize(view.source->members.size());
    view.active_in.resize(view.target->members.size());
    view.pending_out.resize(view.source->members.size());
    view.queued_rows.resize(view.source->members.size());
    factor_views.push_back(std::move(view));
    factor_view_ids.emplace(key, id);
    symbol_factor_views[symbol].push_back(id);
    return id;
  }

  void buildFactorizedPlans(
      const std::vector<std::pair<Id, Id>> &unary_rules,
      const std::vector<std::tuple<Id, Id, Id>> &binary_rules) {
    symbol_factor_views.resize(problem.symbols);
    std::vector<bool> observed(problem.symbols, false);
    for (const Edge &edge : problem.edges)
      observed[edge.symbol] = true;
    for (Id symbol = 0; symbol < problem.symbols; ++symbol)
      if (observed[symbol])
        addFactorView(symbol, symbol, symbol);

    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &[lhs, child] : unary_rules) {
        const auto child_views = symbol_factor_views[child];
        for (Id child_view : child_views) {
          const auto &view = factor_views[child_view];
          const auto before = factor_views.size();
          addFactorView(lhs, view.source_origin, view.target_origin);
          changed = changed || factor_views.size() != before;
        }
      }
      for (const auto &[lhs, left, right] : binary_rules) {
        const auto left_views = symbol_factor_views[left];
        const auto right_views = symbol_factor_views[right];
        for (Id left_view : left_views)
          for (Id right_view : right_views) {
            const auto before = factor_views.size();
            addFactorView(lhs, factor_views[left_view].source_origin,
                          factor_views[right_view].target_origin);
            changed = changed || factor_views.size() != before;
          }
      }
      constexpr Id MAX_FACTOR_VIEWS = 64 * 1024;
      if (factor_views.size() > MAX_FACTOR_VIEWS)
        throw std::runtime_error(
            "endpoint quotient factorized view limit exceeded");
    }

    std::map<std::pair<Id, Id>, std::shared_ptr<const ClassLists>> lifts;
    auto getLift = [&](const std::shared_ptr<const Partition> &fine,
                       const std::shared_ptr<const Partition> &coarse) {
      const std::pair<Id, Id> key{fine->id, coarse->id};
      auto &entry = lifts[key];
      if (!entry)
        entry = std::make_shared<const ClassLists>(lift(*fine, *coarse));
      return entry;
    };
    for (FactorView &view : factor_views) {
      view.global_rows = getLift(sources[view.symbol], view.source);
      view.global_columns = getLift(targets[view.symbol], view.target);
    }

    factor_unit_uses.resize(factor_views.size());
    factor_left_uses.resize(factor_views.size());
    factor_right_uses.resize(factor_views.size());
    for (const auto &[lhs, child] : unary_rules)
      for (Id child_view : symbol_factor_views[child]) {
        const auto &view = factor_views[child_view];
        const Id lhs_view = factor_view_ids.at(
            std::make_tuple(lhs, view.source_origin, view.target_origin));
        factor_unit_uses[child_view].push_back(factor_units.size());
        factor_units.push_back({lhs_view, child_view});
      }

    std::map<std::pair<Id, Id>, std::shared_ptr<const Bridge>> bridges;
    auto getBridge = [&](const std::shared_ptr<const Partition> &left,
                         const std::shared_ptr<const Partition> &right) {
      const std::pair<Id, Id> key{left->id, right->id};
      auto &bridge = bridges[key];
      if (bridge)
        return bridge;
      auto value = std::make_shared<Bridge>();
      value->to_right.resize(left->members.size());
      value->to_left.resize(right->members.size());
      if (left == right) {
        for (Id i = 0; i < left->members.size(); ++i) {
          value->to_right[i].push_back(static_cast<ClassId>(i));
          value->to_left[i].push_back(static_cast<ClassId>(i));
        }
        value->pairs = left->members.size();
      } else {
        std::vector<std::pair<Id, Id>> pairs;
        pairs.reserve(problem.nodes);
        for (Id node = 0; node < problem.nodes; ++node)
          pairs.emplace_back(left->class_of[node], right->class_of[node]);
        sortUnique(pairs);
        value->pairs = pairs.size();
        for (const auto &[l, r] : pairs) {
          value->to_right[l].push_back(static_cast<ClassId>(r));
          value->to_left[r].push_back(static_cast<ClassId>(l));
        }
      }
      bridge = std::move(value);
      return bridge;
    };
    for (const auto &[lhs, left, right] : binary_rules)
      for (Id left_view : symbol_factor_views[left])
        for (Id right_view : symbol_factor_views[right]) {
          const auto &l = factor_views[left_view];
          const auto &r = factor_views[right_view];
          const Id lhs_view = factor_view_ids.at(
              std::make_tuple(lhs, l.source_origin, r.target_origin));
          const Id plan = factor_binaries.size();
          factor_left_uses[left_view].push_back(plan);
          factor_right_uses[right_view].push_back(plan);
          factor_binaries.push_back(
              {lhs_view, left_view, right_view, getBridge(l.target, r.source)});
        }
  }

  void prepare() {
    computeNullable();
    std::vector<std::pair<Id, Id>> unary_rules;
    std::vector<std::tuple<Id, Id, Id>> binary_rules;
    for (const Rule &r : problem.rules) {
      if (r.kind == Rule::Kind::Unary)
        unary_rules.emplace_back(r.lhs, r.left);
      if (r.kind == Rule::Kind::Binary) {
        binary_rules.emplace_back(r.lhs, r.left, r.right);
        // R_A = nullable(A)*I union R_A^+. Identity must never be represented
        // as a complete block: a multi-vertex diagonal is not a rectangle.
        if (nullable[r.right])
          unary_rules.emplace_back(r.lhs, r.left);
        if (nullable[r.left])
          unary_rules.emplace_back(r.lhs, r.right);
      }
    }
    sortUnique(unary_rules);
    sortUnique(binary_rules);
    Lists left_dependencies(problem.symbols),
        right_dependencies(problem.symbols);
    for (auto r : unary_rules) {
      left_dependencies[r.first].push_back(r.second);
      right_dependencies[r.first].push_back(r.second);
    }
    for (auto r : binary_rules) {
      left_dependencies[std::get<0>(r)].push_back(std::get<1>(r));
      right_dependencies[std::get<0>(r)].push_back(std::get<2>(r));
    }
    if (options.partitions == PartitionMode::Grammar) {
      std::vector<bool> observed(problem.symbols, false);
      for (const auto &edge : problem.edges)
        observed[edge.symbol] = true;
      buildPartitions(dependencyClosure(left_dependencies, observed),
                      dependencyClosure(right_dependencies, observed));
    } else {
      buildPartitions({}, {});
    }
    refinePreviousPartitions();

    unit_uses.resize(problem.symbols);
    left_uses.resize(problem.symbols);
    right_uses.resize(problem.symbols);
    relations.resize(problem.symbols);
    for (Id a = 0; a < problem.symbols; ++a) {
      relations[a].known.reset(sources[a]->members.size(),
                               targets[a]->members.size());
      relations[a].active_out.resize(sources[a]->members.size());
      relations[a].active_in.resize(targets[a]->members.size());
      relations[a].pending_out.resize(sources[a]->members.size());
      relations[a].queued_rows.resize(sources[a]->members.size());
    }
    migratePreviousClosure();
    if (options.factorized) {
      buildFactorizedPlans(unary_rules, binary_rules);
      return;
    }
    using Key = std::pair<Id, Id>;
    std::map<Key, std::shared_ptr<const ClassLists>> lifts;
    auto getLift = [&](const std::shared_ptr<const Partition> &fine,
                       const std::shared_ptr<const Partition> &coarse) {
      const Key key{fine->id, coarse->id};
      auto &entry = lifts[key];
      if (!entry) {
        entry = std::make_shared<const ClassLists>(lift(*fine, *coarse));
        ++stats.lifts_built;
      }
      return entry;
    };
    std::map<Key, std::shared_ptr<const Bridge>> bridges;
    for (auto r : unary_rules) {
      Id a = r.first, b = r.second;
      if (a == b)
        continue;
      unit_uses[b].push_back(units.size());
      units.push_back({a, b, getLift(sources[a], sources[b]),
                       getLift(targets[a], targets[b])});
    }
    for (auto r : binary_rules) {
      Id a = std::get<0>(r), b = std::get<1>(r), c = std::get<2>(r);
      const Key key{targets[b]->id, sources[c]->id};
      auto &bridge = bridges[key];
      if (!bridge) {
        auto value = std::make_shared<Bridge>();
        value->to_right.resize(targets[b]->members.size());
        value->to_left.resize(sources[c]->members.size());
        if (targets[b] == sources[c]) {
          for (Id i = 0; i < targets[b]->members.size(); ++i) {
            value->to_right[i].push_back(i);
            value->to_left[i].push_back(i);
          }
          value->pairs = targets[b]->members.size();
        } else {
          std::vector<Key> pairs;
          pairs.reserve(problem.nodes);
          for (Id v = 0; v < problem.nodes; ++v)
            pairs.emplace_back(targets[b]->class_of[v],
                               sources[c]->class_of[v]);
          sortUnique(pairs);
          value->pairs = pairs.size();
          for (auto pair : pairs) {
            value->to_right[pair.first].push_back(pair.second);
            value->to_left[pair.second].push_back(pair.first);
          }
        }
        bridge = std::move(value);
        ++stats.bridges_built;
      }
      addCount(stats.bridge_pairs, bridge->pairs);
      left_uses[b].push_back(binaries.size());
      right_uses[c].push_back(binaries.size());
      binaries.push_back({a,
                          b,
                          c,
                          getLift(sources[a], sources[b]),
                          getLift(targets[a], targets[c]),
                          bridge,
                          {}});
    }
  }

  void enqueuePublished(Id a, Id row, Id column) {
    if (relations[a].join_rows)
      relations[a].join_rows->know(row, column);
    auto &relation = relations[a];
    relation.pending_out[row].push_back(column);
    if (!relation.queued_rows[row]) {
      relation.queued_rows[row] = true;
      worklist.push_back({a, row});
    }
    ++pending_cells;
    ++stats.cells;
    ++stats.worklist_pushes;
    stats.peak_worklist = std::max(stats.peak_worklist, pending_cells);
  }

  bool publish(Id a, Id row, Id column) {
    ++stats.insert_attempts;
    if (!relations[a].known.insert(row, column)) {
      ++stats.duplicate_inserts;
      return false;
    }
    enqueuePublished(a, row, column);
    return true;
  }

  void projectFactorCell(Id view_id, Id row, Id column) {
    const auto &view = factor_views[view_id];
    auto &global = relations[view.symbol];
    for (Id global_row : (*view.global_rows)[row])
      for (Id global_column : (*view.global_columns)[column]) {
        if (!global.known.insert(global_row, global_column))
          continue;
        global.active_out[global_row].push_back(
            static_cast<ClassId>(global_column));
        global.active_in[global_column].push_back(
            static_cast<ClassId>(global_row));
        ++stats.cells;
      }
  }

  void enqueueFactorCell(Id view_id, Id row, Id column) {
    auto &view = factor_views[view_id];
    view.pending_out[row].push_back(static_cast<ClassId>(column));
    if (!view.queued_rows[row]) {
      view.queued_rows[row] = true;
      factor_worklist.push_back({view_id, row});
    }
    ++factor_pending_cells;
    ++stats.worklist_pushes;
    stats.peak_worklist = std::max(stats.peak_worklist, factor_pending_cells);
    projectFactorCell(view_id, row, column);
  }

  bool publishFactor(Id view_id, Id row, Id column) {
    ++stats.insert_attempts;
    if (!factor_views[view_id].known.insert(row, column)) {
      ++stats.duplicate_inserts;
      return false;
    }
    enqueueFactorCell(view_id, row, column);
    return true;
  }

  Count publishFactorMany(Id view_id, Id row,
                          const std::vector<ClassId> &columns) {
    stats.insert_attempts += columns.size();
    const Count inserted =
        factor_views[view_id].known.insertMany(row, columns, [&](Id column) {
          enqueueFactorCell(view_id, row, column);
        });
    stats.duplicate_inserts += columns.size() - inserted;
    return inserted;
  }

  Count publishMany(Id a, Id row, const std::vector<ClassId> &columns) {
    stats.insert_attempts += columns.size();
    const Count inserted = relations[a].known.insertMany(
        row, columns, [&](Id column) { enqueuePublished(a, row, column); });
    stats.duplicate_inserts += columns.size() - inserted;
    return inserted;
  }

  void beginBulkValues() {
    bulk_values.clear();
    if (++bulk_epoch == 0) {
      std::fill(bulk_marks.begin(), bulk_marks.end(), 0);
      ++bulk_epoch;
    }
  }

  void addBulkValue(Id value) {
    if (bulk_marks[value] == bulk_epoch)
      return;
    bulk_marks[value] = bulk_epoch;
    bulk_values.push_back(value);
  }

  bool prepareBinaryOutput(BinaryPlan &plan, Id row, Id column) {
    ++stats.binary_joins;
    const auto &output_rows = (*plan.rows)[row];
    const auto &output_columns = (*plan.columns)[column];
    if (output_rows.size() > 1 || output_columns.size() > 1) {
      if (!plan.expanded_ready) {
        plan.expanded.reset(plan.rows->size(), plan.columns->size());
        plan.expanded_ready = true;
      }
      if (!plan.expanded.insert(row, column)) {
        ++stats.repeated_binary_outputs;
        return false;
      }
    }
    addCount(stats.binary_propagations,
             product(output_rows.size(), output_columns.size()));
    return true;
  }

  void binaryJoin(BinaryPlan &plan, Id row, Id column) {
    if (!prepareBinaryOutput(plan, row, column))
      return;
    for (Id i : (*plan.rows)[row])
      for (Id j : (*plan.columns)[column]) {
        if (publish(plan.lhs, i, j))
          ++stats.successful_binary_propagations;
      }
  }

  void prepareJoinRows(Id a) {
    auto &r = relations[a];
    if (r.tried_join_rows || !r.known.isDense())
      return;
    r.tried_join_rows = true;
    const Id rows = r.active_out.size(), columns = r.active_in.size();
    const Id out_words = (columns + 63) / 64, in_words = (rows + 63) / 64;
    // Bound padded indexes too: a very skinny matrix must stay sparse here.
    constexpr Id MAX_WORDS = 128 * 1024;
    if (!out_words || !in_words || rows > MAX_WORDS / out_words ||
        columns > MAX_WORDS / in_words)
      return;
    const Id words = rows * out_words + 2 * columns * in_words;
    constexpr Id MAX_TOTAL_JOIN_INDEX_WORDS = 1024 * 1024;
    if (words > MAX_TOTAL_JOIN_INDEX_WORDS - join_index_words)
      return;
    join_index_words += words;
    r.join_rows = std::make_unique<JoinRows>(JoinRows{
        out_words, in_words, std::vector<std::uint64_t>(rows * out_words),
        std::vector<std::uint64_t>(columns * in_words),
        std::vector<std::uint64_t>(columns * in_words)});
    r.known.forEach([&](Id row, Id column) { r.join_rows->know(row, column); });
    for (Id row = 0; row < rows; ++row)
      for (Id column : r.active_out[row])
        r.join_rows->activate(row, column);
  }

  bool canBatchJoin(const BinaryPlan &plan, bool from_left) {
    // Identical endpoint partitions make the lifts identities. Union active
    // rows (or columns) and subtract known output with word operations instead
    // of visiting every witness and attempting the same insertion repeatedly.
    if (sources[plan.lhs] != sources[plan.left] ||
        targets[plan.lhs] != targets[plan.right])
      return false;
    const Id other = from_left ? plan.right : plan.left;
    prepareJoinRows(other);
    prepareJoinRows(plan.lhs);
    return relations[other].join_rows && relations[plan.lhs].join_rows;
  }

  void batchJoin(BinaryPlan &plan, const Fact &f, bool from_left) {
    const Id other = from_left ? plan.right : plan.left;
    const auto &input = relations[other].join_rows;
    auto &output = relations[plan.lhs];
    assert(input && output.join_rows);
    const auto &middles = from_left ? plan.bridge->to_right[f.column]
                                    : plan.bridge->to_left[f.row];
    const Id words = from_left ? input->out_words : input->in_words;
    for (Id middle : middles) {
      const auto &values = from_left ? input->out : input->in;
      for (Id w = 0; w < words; ++w) {
        ++stats.binary_join_words;
        const auto candidates = values[middle * words + w];
        const auto known =
            from_left ? output.known.rowWord(f.row, w)
                      : output.join_rows->known_in[f.column * words + w];
        auto delta = candidates & ~known;
        const Count joins = __builtin_popcountll(candidates);
        stats.binary_joins += joins;
        stats.repeated_binary_outputs += joins - __builtin_popcountll(delta);
        while (delta) {
          const Id endpoint = w * 64 + __builtin_ctzll(delta);
          ++stats.binary_propagations;
          if (publish(plan.lhs, from_left ? f.row : endpoint,
                      from_left ? endpoint : f.column))
            ++stats.successful_binary_propagations;
          delta &= delta - 1;
        }
      }
    }
  }

  Count forwardJoinPairs(const BinaryPlan &plan, Id row,
                         const std::vector<ClassId> &delta) const {
    Count pairs = 0;
    for (Id left_column : delta) {
      for (Id middle : plan.bridge->to_right[left_column])
        pairs += relations[plan.right].active_out[middle].size();
      if (plan.right == plan.left) {
        const auto &right_rows = plan.bridge->to_right[left_column];
        if (std::binary_search(right_rows.begin(), right_rows.end(), row))
          pairs += delta.size();
      }
    }
    return pairs;
  }

  void bulkForwardJoin(BinaryPlan &plan, Id row,
                       const std::vector<ClassId> &delta) {
    beginBulkValues();
    auto addOutput = [&](Id column) {
      if (!prepareBinaryOutput(plan, row, column))
        return;
      for (Id output : (*plan.columns)[column])
        addBulkValue(output);
    };
    for (Id left_column : delta) {
      for (Id middle : plan.bridge->to_right[left_column])
        for (Id column : relations[plan.right].active_out[middle])
          addOutput(column);
      if (plan.right == plan.left) {
        const auto &right_rows = plan.bridge->to_right[left_column];
        if (std::binary_search(right_rows.begin(), right_rows.end(), row))
          for (Id column : delta)
            addOutput(column);
      }
    }
    std::sort(bulk_values.begin(), bulk_values.end());
    for (Id output_row : (*plan.rows)[row])
      stats.successful_binary_propagations +=
          publishMany(plan.lhs, output_row, bulk_values);
  }

  Count backwardJoinPairs(const BinaryPlan &plan, Id row,
                          const std::vector<ClassId> &delta) const {
    Count left_cells = 0;
    for (Id middle : plan.bridge->to_left[row])
      left_cells += relations[plan.left].active_in[middle].size();
    return product(left_cells, delta.size());
  }

  void bulkBackwardJoin(BinaryPlan &plan, Id row,
                        const std::vector<ClassId> &delta) {
    for (Id middle : plan.bridge->to_left[row])
      for (Id source : relations[plan.left].active_in[middle]) {
        beginBulkValues();
        for (Id column : delta) {
          if (!prepareBinaryOutput(plan, source, column))
            continue;
          for (Id output : (*plan.columns)[column])
            addBulkValue(output);
        }
        std::sort(bulk_values.begin(), bulk_values.end());
        for (Id output_row : (*plan.rows)[source])
          stats.successful_binary_propagations +=
              publishMany(plan.lhs, output_row, bulk_values);
      }
  }

  void saturateFactorized() {
    stats.input_edges = problem.edges.size();
    for (const Edge &edge : problem.edges) {
      const Id view_id = factor_view_ids.at(
          std::make_tuple(edge.symbol, edge.symbol, edge.symbol));
      const auto &view = factor_views[view_id];
      const Id row = view.source->class_of[edge.source];
      const Id column = view.target->class_of[edge.target];
      if (publishFactor(view_id, row, column)) {
        ++stats.seed_cells;
        addCount(stats.seed_facts,
                 product(view.source->members[row].size(),
                         view.target->members[column].size()));
      }
    }

    bulk_marks.resize(problem.nodes);
    while (!factor_worklist.empty()) {
      const FactorDeltaRow item = factor_worklist.front();
      factor_worklist.pop_front();
      auto &view = factor_views[item.view];
      std::vector<ClassId> delta;
      delta.swap(view.pending_out[item.row]);
      view.queued_rows[item.row] = false;
      factor_pending_cells -= delta.size();
      stats.worklist_pops += delta.size();

      for (Id plan_id : factor_unit_uses[item.view]) {
        const auto &plan = factor_units[plan_id];
        stats.unary_propagations += delta.size();
        stats.successful_unary_propagations +=
            publishFactorMany(plan.lhs_view, item.row, delta);
      }

      for (Id plan_id : factor_left_uses[item.view]) {
        const auto &plan = factor_binaries[plan_id];
        const auto &right = factor_views[plan.right_view];
        beginBulkValues();
        for (Id left_column : delta) {
          for (Id middle : plan.bridge->to_right[left_column]) {
            stats.binary_joins += right.active_out[middle].size();
            for (Id column : right.active_out[middle])
              addBulkValue(column);
          }
          if (plan.right_view == item.view) {
            const auto &right_rows = plan.bridge->to_right[left_column];
            if (std::binary_search(right_rows.begin(), right_rows.end(),
                                   item.row)) {
              stats.binary_joins += delta.size();
              for (Id column : delta)
                addBulkValue(column);
            }
          }
        }
        std::sort(bulk_values.begin(), bulk_values.end());
        stats.binary_propagations += bulk_values.size();
        stats.successful_binary_propagations +=
            publishFactorMany(plan.lhs_view, item.row, bulk_values);
      }

      for (Id plan_id : factor_right_uses[item.view]) {
        const auto &plan = factor_binaries[plan_id];
        const auto &left = factor_views[plan.left_view];
        for (Id middle : plan.bridge->to_left[item.row])
          for (Id source : left.active_in[middle]) {
            stats.binary_joins += delta.size();
            stats.binary_propagations += delta.size();
            stats.successful_binary_propagations +=
                publishFactorMany(plan.lhs_view, source, delta);
          }
      }

      for (Id column : delta) {
        view.active_out[item.row].push_back(column);
        view.active_in[column].push_back(static_cast<ClassId>(item.row));
      }
    }
  }

  void saturate() {
    if (options.factorized) {
      saturateFactorized();
      return;
    }
    bulk_marks.resize(problem.nodes);
    stats.input_edges = problem.edges.size();
    std::vector<Fact> seeds;
    seeds.reserve(problem.edges.size());
    for (const Edge &e : problem.edges)
      seeds.push_back({e.symbol, sources[e.symbol]->class_of[e.source],
                       targets[e.symbol]->class_of[e.target]});
    std::sort(seeds.begin(), seeds.end(), [](const Fact &lhs, const Fact &rhs) {
      return std::tie(lhs.symbol, lhs.row, lhs.column) <
             std::tie(rhs.symbol, rhs.row, rhs.column);
    });
    seeds.erase(std::unique(seeds.begin(), seeds.end(),
                            [](const Fact &lhs, const Fact &rhs) {
                              return lhs.symbol == rhs.symbol &&
                                     lhs.row == rhs.row &&
                                     lhs.column == rhs.column;
                            }),
                seeds.end());
    for (const Fact &seed : seeds) {
      ++stats.seed_cells;
      addCount(stats.seed_facts,
               product(sources[seed.symbol]->members[seed.row].size(),
                       targets[seed.symbol]->members[seed.column].size()));
      // New seed cells enter the delta worklist. Migrated seed cells remain
      // counted above but were already saturated in the previous snapshot.
      (void)publish(seed.symbol, seed.row, seed.column);
    }
    while (!worklist.empty()) {
      const DeltaRow item = worklist.front();
      worklist.pop_front();
      auto &relation = relations[item.symbol];
      std::vector<ClassId> delta;
      delta.swap(relation.pending_out[item.row]);
      relation.queued_rows[item.row] = false;
      pending_cells -= delta.size();
      stats.worklist_pops += delta.size();

      for (Id column : delta) {
        const Fact fact{item.symbol, item.row, column};
        for (Id id : unit_uses[fact.symbol]) {
          const UnaryPlan &plan = units[id];
          for (Id i : (*plan.rows)[fact.row])
            for (Id j : (*plan.columns)[fact.column]) {
              ++stats.unary_propagations;
              if (publish(plan.lhs, i, j))
                ++stats.successful_unary_propagations;
            }
        }
      }

      for (Id id : left_uses[item.symbol]) {
        BinaryPlan &plan = binaries[id];
        if (canBatchJoin(plan, true)) {
          for (Id column : delta)
            batchJoin(plan, {item.symbol, item.row, column}, true);
          if (plan.right == item.symbol)
            for (Id left_column : delta) {
              const auto &right_rows = plan.bridge->to_right[left_column];
              if (std::binary_search(right_rows.begin(), right_rows.end(),
                                     item.row))
                for (Id right_column : delta)
                  binaryJoin(plan, item.row, right_column);
            }
          continue;
        }
        if (forwardJoinPairs(plan, item.row, delta) >= 64) {
          bulkForwardJoin(plan, item.row, delta);
          continue;
        }
        for (Id left_column : delta) {
          for (Id middle : plan.bridge->to_right[left_column])
            for (Id target : relations[plan.right].active_out[middle])
              binaryJoin(plan, item.row, target);
          if (plan.right == item.symbol) {
            const auto &right_rows = plan.bridge->to_right[left_column];
            if (std::binary_search(right_rows.begin(), right_rows.end(),
                                   item.row))
              for (Id right_column : delta)
                binaryJoin(plan, item.row, right_column);
          }
        }
      }

      for (Id id : right_uses[item.symbol]) {
        BinaryPlan &plan = binaries[id];
        if (canBatchJoin(plan, false)) {
          for (Id column : delta)
            batchJoin(plan, {item.symbol, item.row, column}, false);
          continue;
        }
        if (backwardJoinPairs(plan, item.row, delta) >= 64) {
          bulkBackwardJoin(plan, item.row, delta);
          continue;
        }
        for (Id column : delta)
          for (Id middle : plan.bridge->to_left[item.row])
            for (Id source : relations[plan.left].active_in[middle])
              binaryJoin(plan, source, column);
      }

      for (Id column : delta) {
        relation.active_out[item.row].push_back(column);
        relation.active_in[column].push_back(item.row);
        if (relation.join_rows)
          relation.join_rows->activate(item.row, column);
      }
    }
  }

  void count() {
    stats.per_symbol.resize(problem.symbols);
    for (Id a = 0; a < problem.symbols; ++a) {
      SymbolStatistics &s = stats.per_symbol[a];
      s.source_classes = sources[a]->members.size();
      s.target_classes = targets[a]->members.size();
      for (Id i = 0; i < relations[a].active_out.size(); ++i) {
        addCount(s.positive_cells, relations[a].active_out[i].size());
        for (Id j : relations[a].active_out[i])
          addCount(s.positive_facts, product(sources[a]->members[i].size(),
                                             targets[a]->members[j].size()));
      }
      Count positive_diagonal = 0;
      if (sources[a] == targets[a]) {
        for (Id i = 0; i < sources[a]->members.size(); ++i)
          if (relations[a].known.contains(i, i))
            addCount(positive_diagonal, sources[a]->members[i].size());
      } else if (s.positive_cells) {
        for (Id v = 0; v < problem.nodes; ++v)
          positive_diagonal += positiveContains(a, v, v) ? 1 : 0;
      }
      s.logical_facts = s.positive_facts;
      s.diagonal_facts = nullable[a] ? problem.nodes : positive_diagonal;
      if (nullable[a])
        addCount(s.logical_facts, problem.nodes - positive_diagonal);
      addCount(stats.logical_facts, s.logical_facts);
    }
    stats.inferred_facts = stats.logical_facts - stats.seed_facts;
  }

  bool positiveContains(Id a, Id u, Id v) const {
    return relations[a].known.contains(sources[a]->class_of[u],
                                       targets[a]->class_of[v]);
  }

  void solve() {
    if (solved)
      return;
    if (started)
      throw std::logic_error(
          "cannot retry an interrupted endpoint quotient solve");
    started = true;
    auto begin = Clock::now();
    prepare();
    auto prepared = Clock::now();
    saturate();
    auto saturated = Clock::now();
    count();
    auto counted = Clock::now();
    stats.preprocess_ms = milliseconds(begin, prepared);
    stats.saturation_ms = milliseconds(prepared, saturated);
    stats.count_ms = milliseconds(saturated, counted);
    // Queries need partitions and final adjacency, not grammar plans, input
    // copies, worklists, or the temporary refinement-output cache.
    decltype(units)().swap(units);
    decltype(binaries)().swap(binaries);
    Lists().swap(unit_uses);
    Lists().swap(left_uses);
    Lists().swap(right_uses);
    decltype(worklist)().swap(worklist);
    decltype(factor_views)().swap(factor_views);
    Lists().swap(symbol_factor_views);
    decltype(factor_view_ids)().swap(factor_view_ids);
    decltype(factor_units)().swap(factor_units);
    decltype(factor_binaries)().swap(factor_binaries);
    Lists().swap(factor_unit_uses);
    Lists().swap(factor_left_uses);
    Lists().swap(factor_right_uses);
    decltype(factor_worklist)().swap(factor_worklist);
    decltype(problem.edges)().swap(problem.edges);
    decltype(problem.rules)().swap(problem.rules);
    for (auto &relation : relations)
      relation.join_rows.reset();
    solved = true;
  }
};

Solver::Solver(Problem problem, Options options)
    : impl_(new Impl(std::move(problem), options)) {}
Solver::Solver(Problem problem, const Solver &previous, Options options)
    : impl_(new Impl(std::move(problem), options, previous.impl_.get())) {}
Solver::~Solver() = default;
Solver::Solver(Solver &&) noexcept = default;
Solver &Solver::operator=(Solver &&) noexcept = default;

void Solver::solve() { impl_->solve(); }

bool Solver::contains(Id symbol, Id source, Id target) const {
  impl_->requireSolved();
  impl_->requireSymbol(symbol);
  if (source >= impl_->problem.nodes || target >= impl_->problem.nodes)
    throw std::out_of_range("endpoint quotient query node out of range");
  return (source == target && impl_->nullable[symbol]) ||
         impl_->positiveContains(symbol, source, target);
}

bool Solver::isNullable(Id symbol) const {
  impl_->requireSolved();
  impl_->requireSymbol(symbol);
  return impl_->nullable[symbol];
}

const Statistics &Solver::statistics() const {
  impl_->requireSolved();
  return impl_->stats;
}

Id Solver::nodeCount() const { return impl_->problem.nodes; }

bool Solver::visitSuccessors(Id a, Id source,
                             const NodeVisitor &visitor) const {
  impl_->requireSolved();
  impl_->requireSymbol(a);
  if (source >= nodeCount())
    throw std::out_of_range("endpoint quotient source out of range");
  const Id row = impl_->sources[a]->class_of[source];
  for (Id column : impl_->relations[a].active_out[row])
    for (Id target : impl_->targets[a]->members[column])
      if (!visitor(target))
        return false;
  return !impl_->nullable[a] || impl_->positiveContains(a, source, source) ||
         visitor(source);
}

bool Solver::visitPredecessors(Id a, Id target,
                               const NodeVisitor &visitor) const {
  impl_->requireSolved();
  impl_->requireSymbol(a);
  if (target >= nodeCount())
    throw std::out_of_range("endpoint quotient target out of range");
  const Id column = impl_->targets[a]->class_of[target];
  for (Id row : impl_->relations[a].active_in[column])
    for (Id source : impl_->sources[a]->members[row])
      if (!visitor(source))
        return false;
  return !impl_->nullable[a] || impl_->positiveContains(a, target, target) ||
         visitor(target);
}

bool Solver::visitFacts(Id a, const PairVisitor &visitor) const {
  impl_->requireSolved();
  impl_->requireSymbol(a);
  for (Id row = 0; row < impl_->relations[a].active_out.size(); ++row)
    for (Id column : impl_->relations[a].active_out[row])
      for (Id source : impl_->sources[a]->members[row])
        for (Id target : impl_->targets[a]->members[column])
          if (!visitor(source, target))
            return false;
  if (impl_->nullable[a])
    for (Id v = 0; v < nodeCount(); ++v)
      if (!impl_->positiveContains(a, v, v) && !visitor(v, v))
        return false;
  return true;
}

Count Solver::countOffDiagonalUnion(std::vector<Id> symbols) const {
  impl_->requireSolved();
  sortUnique(symbols);
  for (Id a : symbols)
    impl_->requireSymbol(a);
  if (symbols.empty())
    return 0;
  if (symbols.size() == 1) {
    const auto &s = impl_->stats.per_symbol[symbols.front()];
    return s.logical_facts - s.diagonal_facts;
  }

  // Sources with the same tuple of source classes have identical positive
  // successor sets across all selected symbols. Count their union once.
  Lists signatures(nodeCount());
  for (Id v = 0; v < nodeCount(); ++v) {
    signatures[v].reserve(symbols.size());
    for (Id a : symbols)
      signatures[v].push_back(impl_->sources[a]->class_of[v]);
  }
  const Partition common = Partition::fromSignatures(signatures);
  std::vector<Id> marks(nodeCount(), 0);
  Count result = 0;
  for (Id group = 0; group < common.members.size(); ++group) {
    const Id source = common.members[group].front();
    const Id stamp = group + 1;
    Id targets = 0;
    for (Id a : symbols) {
      const Id row = impl_->sources[a]->class_of[source];
      for (Id column : impl_->relations[a].active_out[row])
        for (Id target : impl_->targets[a]->members[column])
          if (marks[target] != stamp) {
            marks[target] = stamp;
            ++targets;
          }
    }
    Count pairs = product(common.members[group].size(), targets);
    for (Id v : common.members[group])
      if (marks[v] == stamp)
        --pairs;
    addCount(result, pairs);
  }
  return result;
}

std::size_t Solver::estimatedPayloadBytes() const {
  impl_->requireSolved();
  auto listsBytes = [](const auto &lists) {
    using List = typename std::decay_t<decltype(lists)>::value_type;
    using Value = typename List::value_type;
    std::size_t bytes = lists.capacity() * sizeof(List);
    for (const auto &list : lists)
      bytes += list.capacity() * sizeof(Value);
    return bytes;
  };
  std::size_t bytes = sizeof(*this) + sizeof(Impl);
  bytes += impl_->grammar_rules.capacity() * sizeof(Rule) +
           impl_->seed_edges.capacity() * sizeof(Edge);
  bytes += (impl_->sources.capacity() + impl_->targets.capacity()) *
           sizeof(std::shared_ptr<const Partition>);
  bytes += impl_->nullable.capacity() / 8;
  bytes += impl_->stats.per_symbol.capacity() * sizeof(SymbolStatistics);
  std::unordered_set<const Partition *> seen;
  for (const auto *partitions : {&impl_->sources, &impl_->targets})
    for (const auto &p : *partitions)
      if (seen.insert(p.get()).second)
        bytes += sizeof(Partition) + p->class_of.capacity() * sizeof(Id) +
                 listsBytes(p->members);
  bytes += impl_->relations.capacity() * sizeof(Relation);
  for (const auto &r : impl_->relations)
    bytes += r.known.payloadBytes() + listsBytes(r.active_out) +
             listsBytes(r.active_in);
  return bytes;
}

void Solver::forEachPositiveRectangle(const RectangleVisitor &visitor) const {
  impl_->requireSolved();
  for (Id a = 0; a < impl_->problem.symbols; ++a)
    for (Id i = 0; i < impl_->relations[a].active_out.size(); ++i)
      for (Id j : impl_->relations[a].active_out[i])
        visitor(a, impl_->sources[a]->members[i],
                impl_->targets[a]->members[j]);
}

void Solver::forEachFact(const FactVisitor &visitor) const {
  forEachPositiveRectangle(
      [&](Id a, const std::vector<Id> &rows, const std::vector<Id> &columns) {
        for (Id u : rows)
          for (Id v : columns)
            visitor(a, u, v);
      });
  for (Id a = 0; a < impl_->problem.symbols; ++a)
    if (impl_->nullable[a])
      for (Id v = 0; v < impl_->problem.nodes; ++v)
        if (!impl_->positiveContains(a, v, v))
          visitor(a, v, v);
}

} // namespace endpoint
} // namespace cfl
} // namespace lotus
