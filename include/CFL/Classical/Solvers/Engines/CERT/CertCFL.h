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

enum class LimitAction { SparseFallback, Throw };
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
  // A count of dense tile records, NOT a limit on total resident memory.
  // Zero means unlimited. Each record is currently 24 bytes on 64-bit hosts.
  std::size_t max_tiles = 4U * 1024U * 1024U;
  std::size_t max_levels = 0;
  // Cumulative dense joins across all levels; zero means unlimited.
  std::uint64_t max_dense_joins = 0;
  LimitAction on_limit = LimitAction::SparseFallback;
  // Concrete facts, including identities, in sparse fallback; zero = unlimited.
  std::size_t max_sparse_facts = 0;
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
  bool used_sparse_fallback = false;
  std::size_t sparse_work_items = 0;
  std::uint64_t sparse_joins = 0;
  std::size_t sparse_facts = 0;
  std::size_t sparse_duplicate_attempts = 0;
};
struct TileSummary {
  bool may = false; // Derivations containing at least one seed edge, not epsilon.
  std::size_t out = 0; // Universal outgoing-degree LOWER bound, not an average.
  std::size_t in = 0;  // Universal incoming-degree LOWER bound.
};

// Immutable snapshot. Copying shares storage; concurrent const access is safe
// provided callbacks do not concurrently mutate their own shared state.
class Result {
public:
  using NodeVisitor = std::function<bool(Node)>;
  using EdgeVisitor = std::function<bool(const Seed &)>;
  Result(const Result &) noexcept = default;
  Result(Result &&) noexcept = default;
  Result &operator=(const Result &) noexcept = default;
  Result &operator=(Result &&) noexcept = default;
  ~Result();

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
  // Diagnostic access to sound summaries, including summaries of explicit seed
  // symbols. After sparse fallback, blocks() is empty and tile() throws.
  const Partition &blocks() const;
  bool nullable(Symbol symbol) const;
  TileSummary tile(Symbol symbol, std::size_t source_block,
                   std::size_t target_block) const;

private:
  struct Impl;
  explicit Result(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
  friend Result solve(const Problem &, const Options &);
};

// Strong exception guarantee: inputs are never changed. No partial result is
// returned on allocation failure, invalid input, or a configured hard limit.
Result solve(const Problem &problem, const Options &options = {});

} // namespace lotus::cfl::classical::engines::cert
