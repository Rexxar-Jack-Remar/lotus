#pragma once

// CERT-CFL's dependency-free C++17 kernel. IDs are dense, zero-based.
// This file intentionally does not depend on LLVM or on Lotus's Relation.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace lotus::cfl::classical::engines::cert {

using Node = std::size_t;
using Symbol = std::uint32_t;
using Partition = std::vector<std::vector<Node>>;

struct Seed {
  Symbol symbol = 0;
  Node source = 0;
  Node target = 0;
  bool operator==(const Seed &other) const noexcept {
    return symbol == other.symbol && source == other.source &&
           target == other.target;
  }
};
struct SeedHash {
  std::size_t operator()(const Seed &seed) const noexcept;
};
struct UnaryRule {
  Symbol lhs = 0;
  Symbol rhs = 0;
};
struct BinaryRule {
  Symbol lhs = 0;
  Symbol first = 0;
  Symbol second = 0;
};
struct Problem {
  std::size_t nodes = 0;
  std::size_t symbols = 0;
  // Seeds can have terminal OR nonterminal labels. Duplicate seeds are ignored.
  std::vector<Seed> seeds;
  std::vector<UnaryRule> unary;
  std::vector<BinaryRule> binary;
  // Explicit epsilon productions; already transitively nullable symbols are
  // also accepted. The kernel computes the complete nullable closure itself.
  std::vector<Symbol> epsilon;
};

class ResourceLimit : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
struct Options {
  // nullopt = all symbols; an empty vector = no required outputs.
  // Unresolved symbols remain queryable with answer(), not with contains().
  std::optional<std::vector<Symbol>> observed;
  // Empty means one block containing all nodes (no blocks when nodes == 0).
  // Otherwise this must be an exact partition, including isolated nodes.
  Partition initial_partition;
  bool majority_threshold = true;
  // Exact seeds + symbolic identity suffice for symbols without any unary or
  // binary defining rules. Do not refine merely to rediscover input edges.
  bool keep_seed_symbols_explicit = true;
  // A count of active tile records, NOT a limit on total resident memory.
  // Zero means unlimited. Large logical tile spaces are stored sparsely, so
  // the limit applies to materialized records rather than the logical matrix.
  std::size_t max_tiles = 0;
  std::size_t max_levels = 0;
  // Cumulative dense joins across all levels; zero means unlimited.
  std::uint64_t max_dense_joins = 0;
};
struct Statistics {
  std::size_t levels = 0;
  std::size_t final_blocks = 0;
  std::size_t peak_tiles = 0;
  std::size_t explicit_seed_symbols = 0;
  std::size_t queue_pops = 0;
  std::size_t peak_queue = 0;
  std::uint64_t joins = 0;
  std::uint64_t updates = 0;
  std::uint64_t overlap_promotions = 0;
  std::uint64_t genuine_cardinality_promotions = 0;
  std::size_t duplicate_attempts = 0;
  std::size_t facts = 0;
};
struct TileSummary {
  bool may = false; // Derivations containing at least one seed edge, not epsilon.
  std::size_t out = 0; // Universal outgoing-degree LOWER bound, not an average.
  std::size_t in = 0;  // Universal incoming-degree LOWER bound.
};

// Mutable monotone result. extend() adds node/seed deltas in place.
class Result {
public:
  using NodeVisitor = std::function<bool(Node)>;
  using EdgeVisitor = std::function<bool(const Seed &)>;
  Result(const Result &) = delete;
  Result(Result &&) noexcept;
  Result &operator=(const Result &) = delete;
  Result &operator=(Result &&) noexcept;
  ~Result();

  void extend(const Problem &delta_problem, const Options &options = {});

  std::size_t nodeCount() const;
  std::size_t symbolCount() const;
  std::size_t blockCount() const;
  bool resolved(Symbol symbol) const;
  // nullopt is UNKNOWN, never false. All IDs are range-checked.
  std::optional<bool> answer(Symbol symbol, Node source, Node target) const;
  bool contains(Symbol symbol, Node source, Node target) const;
  // Exact traversal/counts throw logic_error for an unresolved symbol.
  // Return false on callback cancellation, true after complete traversal.
  bool visitSuccessors(Symbol symbol, Node source, const NodeVisitor &visitor) const;
  bool visitPredecessors(Symbol symbol, Node target, const NodeVisitor &visitor) const;
  bool visitEdges(Symbol symbol, const EdgeVisitor &visitor) const;
  bool visitEdges(const EdgeVisitor &visitor) const;
  std::size_t edgeCount(Symbol symbol) const;
  std::size_t edgeCount() const;
  // Count the union across labels, excluding (u,u), without duplicate counting.
  std::size_t countOffDiagonalUnion(std::vector<Symbol> symbols) const;
  std::size_t estimatedPayloadBytes() const;
  const Statistics &statistics() const;
  // Diagnostic access to sound summaries, including explicit seed symbols.
  const Partition &blocks() const;
  bool nullable(Symbol symbol) const;
  TileSummary tile(Symbol symbol, std::size_t source_block,
                   std::size_t target_block) const;

private:
  struct Impl;
  explicit Result(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend Result solve(const Problem &, const Options &);
};

Result solve(const Problem &problem, const Options &options = {});

} // namespace lotus::cfl::classical::engines::cert
