#ifndef LOTUS_CFL_CLASSICAL_ENDPOINT_QUOTIENT_H
#define LOTUS_CFL_CLASSICAL_ENDPOINT_QUOTIENT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace lotus {
namespace cfl {
namespace endpoint {

using Id = std::size_t;
using Count = std::uint64_t;

struct Edge {
  Id source;
  Id symbol;
  Id target;
};

// Normalized productions. Symbols may also label input edges: such edges are
// axioms. Conventional terminal/nonterminal grammars are a special case.
struct Rule {
  enum class Kind { Epsilon, Unary, Binary };
  Kind kind;
  Id lhs;
  Id left = 0;
  Id right = 0;

  static Rule epsilon(Id lhs) { return {Kind::Epsilon, lhs, 0, 0}; }
  static Rule unary(Id lhs, Id rhs) { return {Kind::Unary, lhs, rhs, 0}; }
  static Rule binary(Id lhs, Id left, Id right) {
    return {Kind::Binary, lhs, left, right};
  }
};

struct Problem {
  Id nodes = 0;
  Id symbols = 0;
  std::vector<Edge> edges;
  std::vector<Rule> rules;

  // Throws std::invalid_argument for out-of-range IDs or an invalid rule kind.
  void validate() const;
};

enum class PartitionMode {
  Grammar,  // Separate FIRST/LAST-derived partitions for every symbol.
  Global,   // Ablation: every symbol observes every edge label.
  Singleton // Ablation: no endpoint compression.
};

struct Options {
  PartitionMode partitions = PartitionMode::Grammar;
  /// Preserve production-local endpoint factors during saturation while
  /// maintaining the exact symbol-global relation for public queries.
  bool factorized = false;
};

struct SymbolStatistics {
  Count source_classes = 0;
  Count target_classes = 0;
  Count positive_cells = 0;
  Count positive_facts = 0;
  Count logical_facts = 0;
  Count diagonal_facts = 0;
};

/// Work attributed to one entry in Problem::rules.  The vector in Statistics
/// is index-aligned with Problem::rules so adapters can attach grammar symbol
/// names without making the core solver depend on Grammar.
struct RuleStatistics {
  Count delta_rows = 0;
  Count delta_cells = 0;
  Count joins = 0;
  Count propagations = 0;
  Count successful_propagations = 0;
  Count repeated_outputs = 0;
  Count join_word_operations = 0;
};

struct Statistics {
  Count input_edges = 0;
  Count seed_cells = 0;
  Count seed_facts = 0;
  Count cells = 0;
  Count nullable_symbols = 0;
  Count logical_facts = 0; // Includes axioms and epsilon; no duplicates.
  Count inferred_facts = 0;
  Count insert_attempts = 0;
  Count duplicate_inserts = 0;
  Count worklist_pushes = 0;
  Count worklist_pops = 0;
  Count peak_worklist = 0;
  Count binary_joins = 0;       // Compatible ordered pairs of positive cells.
  Count unary_propagations = 0; // Candidate parent cells after refinement.
  Count binary_propagations = 0;
  Count successful_unary_propagations = 0;
  Count successful_binary_propagations = 0;
  Count bridge_pairs = 0; // Sum over distinct normalized binary productions.
  Count partitions_built = 0;
  Count bridges_built = 0;
  Count lifts_built = 0;
  Count repeated_binary_outputs = 0;
  Count binary_join_words =
      0; // Word operations used instead of individual joins.
  double preprocess_ms = 0;
  double saturation_ms = 0;
  double count_ms = 0;
  std::vector<SymbolStatistics> per_symbol;
  std::vector<RuleStatistics> per_rule;
};

// All IDs are dense in [0,nodes) or [0,symbols). The problem is owned by value.
class Solver {
public:
  explicit Solver(Problem problem, Options options = {});
  /// Builds an updated exact snapshot for the same grammar and a monotone
  /// superset of input edges. New endpoint partitions are refined by the
  /// previous partitions, so old rectangles can be migrated without replaying
  /// old grammar work.
  Solver(Problem problem, const Solver &previous, Options options = {});
  ~Solver();
  Solver(Solver &&) noexcept;
  Solver &operator=(Solver &&) noexcept;
  Solver(const Solver &) = delete;
  Solver &operator=(const Solver &) = delete;

  // Computes the exact least fixed point. A repeated call is a no-op.
  void solve();
  bool contains(Id symbol, Id source, Id target) const;
  bool isNullable(Id symbol) const;
  const Statistics &statistics() const;
  Id nodeCount() const;
  std::size_t estimatedPayloadBytes() const;

  // Streaming queries on the solved snapshot. Return false if the visitor
  // stops traversal by returning false. No concrete result vector is built.
  using NodeVisitor = std::function<bool(Id)>;
  using PairVisitor = std::function<bool(Id, Id)>;
  bool visitSuccessors(Id symbol, Id source, const NodeVisitor &visitor) const;
  bool visitPredecessors(Id symbol, Id target,
                         const NodeVisitor &visitor) const;
  bool visitFacts(Id symbol, const PairVisitor &visitor) const;

  // Counts distinct (source,target) pairs across symbols, excluding self pairs.
  // Uses common source classes and a reusable target marking array.
  Count countOffDiagonalUnion(std::vector<Id> symbols) const;

  // Visits each logical fact exactly once. Expansion is output-sensitive and
  // is intentionally NOT performed by solve() or statistics().
  using FactVisitor = std::function<void(Id, Id, Id)>;
  void forEachFact(const FactVisitor &visitor) const;

  // Visits disjoint rectangles of POSITIVE-LENGTH reachability only. Epsilon
  // diagonals are separate, accessible through isNullable()/contains().
  using RectangleVisitor =
      std::function<void(Id, const std::vector<Id> &, const std::vector<Id> &)>;
  void forEachPositiveRectangle(const RectangleVisitor &visitor) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace endpoint
} // namespace cfl
} // namespace lotus

#endif
