/**
 * @file PDGDiff.h
 * @brief Structural differencing of Program Dependency Graph sub-graphs.
 *
 * This file implements semantic differencing of PDG sub-graphs, inspired by:
 *
 * - Horwitz, "Identifying the Semantic and Textual Differences Between Two
 *   Versions of a Program", PLDI 1990.
 * - Jackson & Ladd, "Semantic Diff: A Tool for Summarizing the Effects of
 *   Modifications", ICSM 1994.
 *
 * The core idea: given two slices (or arbitrary sub-graphs) S1 and S2 of a
 * PDG, determine which nodes/edges are added, removed, or preserved.  This is
 * valuable for:
 *
 * 1. Change-impact analysis -- after a code change, diff the PDG before and
 *    after to see which dependences changed.
 * 2. Regression testing prioritization -- nodes that appear in the diff are
 *    more likely to be affected by a change.
 * 3. Understanding program evolution -- comparing slices across versions.
 *
 * Two sub-graphs are compared structurally: nodes are matched by their LLVM
 * Value identity (pointer equality in the same module, or by instruction
 * string comparison across modules), and edges are matched by their
 * (src, dst, type) triple.
 */

#pragma once
#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/PDGNode.h"

#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pdg {

// ============================================================================
// Diff result types
// ============================================================================

/**
 * @brief Classification of a diff element.
 */
enum class DiffKind {
  ADDED,     ///< Present only in the "new" sub-graph.
  REMOVED,   ///< Present only in the "old" sub-graph.
  PRESERVED  ///< Present in both sub-graphs.
};

/**
 * @brief A single node diff entry.
 */
struct NodeDiffEntry {
  Node *node = nullptr;
  DiffKind kind = DiffKind::PRESERVED;
};

/**
 * @brief A single edge diff entry.
 */
struct EdgeDiffEntry {
  Edge *edge = nullptr;
  DiffKind kind = DiffKind::PRESERVED;
};

/**
 * @brief Full diff result between two PDG sub-graphs.
 *
 * Contains the complete classification of all nodes and edges from both
 * sub-graphs, indicating whether each element was ADDED, REMOVED, or PRESERVED.
 * Provides convenience accessors for quick statistics.
 *
 * @see PDGDiff::diff() for how to compute this result
 * @see PDGDiff::printDiffSummary() for human-readable output
 */
struct PDGDiffResult {
  /// Nodes classified by diff kind.
  std::vector<NodeDiffEntry> node_diffs;
  /// Edges classified by diff kind.
  std::vector<EdgeDiffEntry> edge_diffs;

  /// Convenience accessors.
  size_t numAddedNodes() const;
  size_t numRemovedNodes() const;
  size_t numPreservedNodes() const;
  size_t numAddedEdges() const;
  size_t numRemovedEdges() const;
  size_t numPreservedEdges() const;

  /// True if the two sub-graphs are structurally identical.
  bool isIdentical() const;
};

/**
 * @brief Diagnostics for the diff computation.
 */
struct PDGDiffDiagnostics {
  size_t old_nodes = 0;
  size_t new_nodes = 0;
  size_t old_edges = 0;
  size_t new_edges = 0;
  double diff_time_ms = 0.0;
};

// ============================================================================
// PDGDiff
// ============================================================================

/**
 * @brief Structural differencing engine for PDG sub-graphs.
 *
 * Compares two PDG sub-graphs (e.g., slices, function bodies, or arbitrary
 * node sets) and identifies which nodes and edges are ADDED, REMOVED, or
 * PRESERVED between them. This enables semantic change analysis at the
 * dependence-graph level rather than just textual diff.
 *
 * **Key applications:**
 * 1. **Change-impact analysis**: After modifying code, diff the PDG before/after
 *    to see which dependences changed, not just which lines changed.
 * 2. **Regression test prioritization**: Tests covering nodes/edges in the
 *    diff are more likely to reveal bugs.
 * 3. **Program evolution**: Compare slices across versions to understand how
 *    program structure evolved.
 * 4. **Refactoring validation**: Verify that a refactoring preserved semantic
 *    structure (edges preserved) even if syntax changed.
 *
 * **Matching strategies:**
 * - **Pointer equality** (default): Fast, works for same-module comparisons
 * - **Instruction string matching**: For cross-module comparisons where nodes
 *   represent semantically equivalent code but are different objects
 * - **Custom matcher**: Implement domain-specific matching logic
 *
 * @see pdg::Slicing for computing slices to diff
 * @see pdg::PDGDiffResult for the diff result structure
 *
 * Example usage:
 * @code
 *   // Get two slices (or any two sub-graphs)
 *   pdg::ForwardSlicing slicer(pdg);
 *   auto slice_v1 = slicer.computeSlice(criteria_v1);
 *   auto slice_v2 = slicer.computeSlice(criteria_v2);
 *
 *   PDGDiff differ;
 *   PDGDiffDiagnostics diag;
 *   auto result = differ.diff(slice_v1, slice_v2, {}, &diag);
 *
 *   if (!result.isIdentical()) {
 *     errs() << "Added nodes: " << result.numAddedNodes() << "\n";
 *     errs() << "Removed nodes: " << result.numRemovedNodes() << "\n";
 *     PDGDiff::printDiffSummary(result, "Version comparison");
 *   }
 * @endcode
 */
class PDGDiff {
public:
  using NodeSet = std::set<Node *>;

  /**
   * @brief Node matching function type.
   *
   * Given a node from the "old" graph and a node from the "new" graph,
   * returns true if they should be considered the same program element.
   * The default matcher uses pointer equality (same module).
   */
  using NodeMatcher = std::function<bool(Node *, Node *)>;

  /**
   * @brief Default constructor (uses pointer-equality matching).
   */
  PDGDiff() = default;

  /**
   * @brief Constructor with custom node matcher.
   * @param matcher Custom function to determine node identity
   */
  explicit PDGDiff(NodeMatcher matcher) : _matcher(std::move(matcher)) {}

  /**
   * @brief Diff two node sets (sub-graphs).
   *
   * Compares the two node sets and their induced edges, classifying each
   * node and edge as ADDED (only in new), REMOVED (only in old), or PRESERVED
   * (in both). Edges are collected from the induced sub-graph of each node set
   * (i.e., only edges where both endpoints are in the node set).
   *
   * **Time complexity:** O(N + E) where N is the number of nodes and E is
   * the number of edges in the union of both sub-graphs.
   *
   * @param old_nodes Nodes in the "old" sub-graph (baseline for comparison)
   * @param new_nodes Nodes in the "new" sub-graph (to compare against old)
   * @param diagnostics Optional output for timing and size statistics
   * @return PDGDiffResult containing classified nodes and edges
   *
   * @note Uses the node matcher specified in the constructor (default:
   *       pointer equality). For cross-module comparison, use a custom matcher.
   */
  PDGDiffResult diff(const NodeSet &old_nodes, const NodeSet &new_nodes,
                     PDGDiffDiagnostics *diagnostics = nullptr);

  /**
   * @brief Diff two node sets, considering only specific edge types.
   *
   * Same as diff() but filters edges by type. Useful for:
   * - Comparing only data dependencies (ignore control dependencies)
   * - Comparing only control dependencies (ignore data dependencies)
   * - Focusing on specific edge types (e.g., parameter edges)
   *
   * @param old_nodes   Nodes in the "old" sub-graph
   * @param new_nodes   Nodes in the "new" sub-graph
   * @param edge_types  Edge types to include in the comparison (empty = all types)
   * @param diagnostics Optional diagnostics output
   * @return The diff result (edge_diffs will only contain edges of specified types)
   *
   * Example:
   * @code
   *   // Compare only data dependencies
   *   auto data_edges = SlicingUtils::getDataDependencyEdges();
   *   auto result = differ.diff(old_nodes, new_nodes, data_edges);
   * @endcode
   */
  PDGDiffResult diff(const NodeSet &old_nodes, const NodeSet &new_nodes,
                     const std::set<EdgeType> &edge_types,
                     PDGDiffDiagnostics *diagnostics = nullptr);

  /**
   * @brief Print a human-readable diff summary to stderr.
   * @param result The diff result to summarize
   * @param label  Optional label for the diff
   */
  static void printDiffSummary(const PDGDiffResult &result,
                               const std::string &label = "PDG Diff");

  /**
   * @brief Get statistics about the diff.
   * @param result The diff result
   * @return Map of statistic names to values
   */
  static std::unordered_map<std::string, size_t>
  getDiffStatistics(const PDGDiffResult &result);

  // --------------------------------------------------------------------------
  // Convenience matchers
  // --------------------------------------------------------------------------

  /**
   * @brief Pointer-equality matcher (for same-module comparisons).
   */
  static bool pointerEqualityMatcher(Node *a, Node *b) { return a == b; }

  /**
   * @brief Instruction-string matcher (for cross-module comparisons).
   *
   * Two nodes match if their LLVM IR textual representations are identical and
   * they have the same GraphNodeType. This enables comparing PDGs from
   * different compilation units or different versions of the same program.
   *
   * **Use when:**
   * - Comparing PDGs from different modules
   * - Comparing PDGs from different program versions
   * - Nodes represent semantically equivalent code but are different objects
   *
   * **Limitations:**
   * - May produce false matches if IR formatting differs
   * - Slower than pointer equality (requires string comparison)
   * - May miss matches if instruction order changed but semantics preserved
   *
   * @param a Node from the "old" graph
   * @param b Node from the "new" graph
   * @return True if nodes represent the same instruction/value
   *
   * Example:
   * @code
   *   PDGDiff differ(PDGDiff::instructionStringMatcher);
   *   auto result = differ.diff(old_module_nodes, new_module_nodes);
   * @endcode
   */
  static bool instructionStringMatcher(Node *a, Node *b);

private:
  NodeMatcher _matcher;

  /**
   * @brief Collect edges induced by a node set, optionally filtered by type.
   */
  static std::set<Edge *>
  collectInducedEdges(const NodeSet &nodes,
                      const std::set<EdgeType> &edge_types);
};

} // namespace pdg
