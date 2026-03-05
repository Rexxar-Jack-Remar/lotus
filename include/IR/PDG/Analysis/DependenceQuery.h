/**
 * @file DependenceQuery.h
 * @brief Pairwise dependence queries, transitive closure, and dependence
 *        distance computation over the Program Dependency Graph (PDG).
 *
 * This file provides a collection of classic PDG-level analyses:
 *
 * 1. **PairwiseDependence** -- given two nodes answer whether a data, control,
 *    or transitive dependence exists, and enumerate witness paths.
 *    (Ferrante, Ottenstein & Warren, TOPLAS 1987)
 *
 * 2. **TransitiveClosure** -- materialize the transitive closure of selected
 *    edge types so that subsequent reachability queries are O(1).
 *    (Horwitz, Reps & Binkley, TOPLAS 1990)
 *
 * 3. **DependenceDistance** -- compute the shortest-path distance between two
 *    nodes (number of edges) along selected edge types.  Useful for ranking
 *    and prioritizing dependence chains in program understanding tools.
 */

#pragma once
#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/PDGNode.h"

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pdg {

// ============================================================================
// PairwiseDependence
// ============================================================================

/**
 * @brief Result of a pairwise dependence query.
 *
 * Contains complete information about a dependence relationship between two
 * nodes, including whether it exists, whether it's direct or transitive,
 * and a witness path demonstrating the dependence.
 *
 * @see PairwiseDependence::query() for how to obtain this result
 */
struct DependenceResult {
  /// True if any dependence exists from source to target.
  bool has_dependence = false;

  /// True if a *direct* (single-edge) dependence exists.
  bool is_direct = false;

  /// True if only a *transitive* (multi-edge) dependence exists.
  bool is_transitive = false;

  /// Edge types that participate in the shortest witness path.
  std::vector<EdgeType> witness_edge_types;

  /// Shortest witness path (node sequence from source to target).
  std::vector<Node *> witness_path;

  /// Number of edges in the shortest witness path (0 if no dependence).
  size_t distance = 0;
};

/**
 * @brief Pairwise dependence queries over the PDG.
 *
 * Answers "is node A dependent on node B?" with respect to a configurable set
 * of edge types.  Supports both forward (A -> B) and backward (B -> A) queries.
 *
 * This class provides the fundamental building block for many PDG analyses:
 * - Impact analysis: "What nodes are affected by this change?"
 * - Debugging: "Where did this value come from?"
 * - Program understanding: "What are all the paths connecting these two nodes?"
 *
 * @note For repeated queries over the same graph, consider using
 *       TransitiveClosure for O(1) queries after an O(N^2) preprocessing step.
 *
 * @see TransitiveClosure for pre-computed reachability
 * @see DependenceDistance for shortest-path distances
 * @see pdg::Slicing for full forward/backward slices
 *
 * Example usage:
 * @code
 *   PairwiseDependence dep(pdg);
 *   DependenceResult result = dep.query(nodeA, nodeB);
 *   if (result.has_dependence) {
 *     errs() << "Distance: " << result.distance << "\n";
 *     errs() << "Path length: " << result.witness_path.size() << "\n";
 *   }
 * @endcode
 */
class PairwiseDependence {
public:
  using NodeSet = std::set<Node *>;
  using DirectDepMap = std::unordered_map<Node *, std::set<EdgeType>>;

  /**
   * @brief Constructor
   * @param pdg Reference to the program dependency graph
   */
  explicit PairwiseDependence(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Query whether @p target is forward-reachable from @p source.
   *
   * Performs a BFS from @p source to @p target, returning detailed information
   * about the shortest witness path if one exists. The result includes:
   * - Whether a dependence exists (direct or transitive)
   * - The shortest witness path as a sequence of nodes
   * - The edge types along that path
   * - The distance (number of edges)
   *
   * @param source  Source node (starting point of the query)
   * @param target  Target node (destination to check reachability to)
   * @param edge_types Allowed edge types (empty set = all types allowed).
   *                   Use this to restrict to data-only, control-only, etc.
   * @return DependenceResult describing the dependence, or has_dependence=false
   *         if no path exists
   *
   * @note This is O(V+E) in the worst case. For many queries, use
   *       TransitiveClosure instead.
   */
  DependenceResult query(Node &source, Node &target,
                         const std::set<EdgeType> &edge_types = {});

  /**
   * @brief Query all direct (single-edge) dependences from @p node.
   *
   * Returns all immediate neighbors connected by a single edge. This is
   * O(degree) and much faster than transitive queries. Useful for:
   * - Inspecting immediate data/control dependencies
   * - Building adjacency lists for custom traversals
   * - Understanding local graph structure
   *
   * @param node  The node to inspect
   * @param forward True to get outgoing dependences (successors),
   *                false to get incoming dependences (predecessors)
   * @param edge_types Allowed edge types (empty = all types)
   * @return Map from neighbor node to the set of edge types connecting them
   *
   * Example:
   * @code
   *   auto deps = dep.directDependences(node, true);
   *   for (auto &[neighbor, edge_types] : deps) {
   *     for (auto edge_type : edge_types)
   *       errs() << "Direct dependence via " << edge_type << "\n";
   *   }
   * @endcode
   */
  DirectDepMap directDependences(Node &node, bool forward,
                                 const std::set<EdgeType> &edge_types = {});

  /**
   * @brief Enumerate *all* shortest witness paths between two nodes.
   *
   * Returns all shortest-length paths from @p source to @p target. The
   * returned paths all have the same length (the BFS distance). Useful for
   * program understanding when multiple independent dependence chains of equal
   * length exist.
   *
   * @param source     Source node
   * @param target     Target node
   * @param edge_types Allowed edge types (empty = all)
   * @param max_paths  Cap on paths returned (0 = unlimited)
   * @return Vector of node-sequence paths
   */
  std::vector<std::vector<Node *>>
  allShortestPaths(Node &source, Node &target,
                   const std::set<EdgeType> &edge_types = {},
                   size_t max_paths = 0);

private:
  GenericGraph &_pdg;
};

// ============================================================================
// TransitiveClosure
// ============================================================================

/**
 * @brief Diagnostics for transitive closure computation.
 */
struct TransitiveClosureDiagnostics {
  /// Number of nodes in the closure graph.
  size_t num_nodes = 0;
  /// Number of reachable pairs (entries in the closure matrix).
  size_t num_reachable_pairs = 0;
  /// Wall-clock milliseconds to build the closure.
  double build_time_ms = 0.0;
};

/**
 * @brief Transitive closure of the PDG restricted to selected edge types.
 *
 * Pre-computes reachability so that subsequent queries are O(1).  Based on the
 * transitive-closure construction used in interprocedural slicing
 * (Horwitz, Reps & Binkley, TOPLAS 1990).
 *
 * **When to use:**
 * - You need to perform many reachability queries over the same sub-graph
 * - The sub-graph is small enough that O(N^2) memory is acceptable
 * - You want O(1) query performance after preprocessing
 *
 * **When NOT to use:**
 * - Very large graphs (memory cost is quadratic)
 * - One-off queries (PairwiseDependence is simpler)
 * - Dynamic graphs that change frequently (rebuild cost is high)
 *
 * **Memory cost:** O(N^2) in the worst case. For a graph with 1000 nodes,
 * expect ~8MB of memory (1000^2 * 8 bytes per pointer).
 *
 * @see PairwiseDependence for one-off queries without preprocessing
 * @see pdg::Slicing for full slice computation
 *
 * Example usage:
 * @code
 *   TransitiveClosure closure(pdg);
 *   closure.build(subgraph_nodes);  // O(N^2) preprocessing
 *   if (closure.canReach(nodeA, nodeB)) {  // O(1) query
 *     auto reachable = closure.getReachableSet(nodeA);
 *   }
 * @endcode
 */
class TransitiveClosure {
public:
  using NodeSet = std::set<Node *>;

  /**
   * @brief Constructor -- does *not* build the closure yet.
   * @param pdg Reference to the program dependency graph
   */
  explicit TransitiveClosure(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Build the closure over the full graph.
   * @param edge_types Allowed edge types (empty = all)
   * @param diagnostics Optional output for diagnostics
   */
  void build(const std::set<EdgeType> &edge_types = {},
             TransitiveClosureDiagnostics *diagnostics = nullptr);

  /**
   * @brief Build the closure restricted to a sub-graph.
   * @param subgraph_nodes Nodes to include
   * @param edge_types     Allowed edge types (empty = all)
   * @param diagnostics    Optional diagnostics output
   */
  void build(const NodeSet &subgraph_nodes,
             const std::set<EdgeType> &edge_types = {},
             TransitiveClosureDiagnostics *diagnostics = nullptr);

  /**
   * @brief O(1) reachability query after build().
   * @param source Source node
   * @param target Target node
   * @return True if @p target is reachable from @p source
   */
  bool canReach(Node &source, Node &target) const;

  /**
   * @brief Get the set of nodes reachable from @p source.
   * @param source Source node
   * @return Set of reachable nodes (empty if source not in closure)
   */
  NodeSet getReachableSet(Node &source) const;

  /**
   * @brief Get the set of nodes that can reach @p target.
   * @param target Target node
   * @return Set of nodes that can reach target
   */
  NodeSet getPredecessorSet(Node &target) const;

  /**
   * @brief Check whether the closure has been built.
   */
  bool isBuilt() const { return _is_built; }

  /**
   * @brief Reset the closure (free memory).
   */
  void reset();

private:
  GenericGraph &_pdg;
  bool _is_built = false;

  /// Forward closure: node -> set of nodes reachable from it.
  std::unordered_map<Node *, std::unordered_set<Node *>> _forward;
  /// Reverse closure: node -> set of nodes that can reach it.
  std::unordered_map<Node *, std::unordered_set<Node *>> _reverse;
};

// ============================================================================
// DependenceDistance
// ============================================================================

/**
 * @brief Shortest-path dependence distance between PDG nodes.
 *
 * Computes the minimum number of edges on a path from source to target,
 * restricted to the given edge types.  Optionally computes single-source
 * shortest distances from a node to all other nodes.
 *
 * **Use cases:**
 * - Ranking dependence chains by "closeness" (shorter = more direct)
 * - Finding the most direct path between two program points
 * - Computing graph metrics (eccentricity, diameter)
 * - Prioritizing analysis targets (closer nodes may be more relevant)
 *
 * **Algorithm:** Uses BFS (breadth-first search), which guarantees shortest
 * paths in unweighted graphs. Time complexity is O(V+E) per query.
 *
 * @see PairwiseDependence for full path information (not just distance)
 * @see TransitiveClosure for O(1) reachability queries
 *
 * Example usage:
 * @code
 *   DependenceDistance dist(pdg);
 *   size_t d = dist.distance(nodeA, nodeB);
 *   if (d != SIZE_MAX) {
 *     errs() << "Shortest path: " << d << " edges\n";
 *   }
 *   // Get all distances from a node
 *   auto distances = dist.forwardDistances(nodeA);
 *   for (auto [node, distance] : distances) {
 *     errs() << "Distance to " << node << ": " << distance << "\n";
 *   }
 * @endcode
 */
class DependenceDistance {
public:
  using NodeSet = std::set<Node *>;
  using DistanceMap = std::unordered_map<Node *, size_t>;

  /**
   * @brief Constructor
   * @param pdg Reference to the program dependency graph
   */
  explicit DependenceDistance(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Compute shortest distance from @p source to @p target.
   * @param source Source node
   * @param target Target node
   * @param edge_types Allowed edge types (empty = all)
   * @return Distance in edges, or SIZE_MAX if unreachable
   */
  size_t distance(Node &source, Node &target,
                  const std::set<EdgeType> &edge_types = {});

  /**
   * @brief Single-source shortest distances (forward).
   * @param source     Source node
   * @param edge_types Allowed edge types (empty = all)
   * @param max_depth  Depth limit (0 = unlimited)
   * @return Map from reachable node to its distance from @p source
   */
  DistanceMap forwardDistances(Node &source,
                               const std::set<EdgeType> &edge_types = {},
                               size_t max_depth = 0);

  /**
   * @brief Single-source shortest distances (backward).
   * @param target     Target node
   * @param edge_types Allowed edge types (empty = all)
   * @param max_depth  Depth limit (0 = unlimited)
   * @return Map from reachable node to its distance to @p target
   */
  DistanceMap backwardDistances(Node &target,
                                const std::set<EdgeType> &edge_types = {},
                                size_t max_depth = 0);

  /**
   * @brief Eccentricity of a node: maximum shortest distance to any reachable
   * node.
   *
   * The eccentricity is the length of the longest shortest path from @p node
   * to any other reachable node. This is a graph-theoretic metric useful for:
   * - Identifying "central" nodes (low eccentricity = well-connected)
   * - Finding "peripheral" nodes (high eccentricity = far from others)
   * - Computing graph diameter (max eccentricity over all nodes)
   *
   * @param node Node to query
   * @param edge_types Allowed edge types (empty = all)
   * @return Eccentricity (0 if no successors, SIZE_MAX if unreachable nodes
   * exist)
   *
   * @note This requires computing distances to all reachable nodes, so it's
   *       O(V+E) in the worst case.
   */
  size_t eccentricity(Node &node, const std::set<EdgeType> &edge_types = {});

private:
  GenericGraph &_pdg;

  /**
   * @brief Templated BFS for single-source distances.
   * @tparam GetEdgesFunc  Returns edge set from a node
   * @tparam GetNeighborFunc  Returns the neighbor from an edge
   */
  template <typename GetEdgesFunc, typename GetNeighborFunc>
  DistanceMap computeDistances(Node &start,
                               const std::set<EdgeType> &edge_types,
                               size_t max_depth, GetEdgesFunc get_edges,
                               GetNeighborFunc get_neighbor);
};

} // namespace pdg
