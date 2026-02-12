/**
 * @file ThinSlicing.h
 * @brief Thin slicing implementation based on EECS-2006-184 paper
 *
 * This file implements thin slicing as described in:
 * "Thin Slicing" by Sridharan, Fink, and Bodik (PLDI 2007 / EECS-2006-184)
 *
 * Thin slicing produces smaller, more relevant slices by:
 * 1. Excluding control dependencies entirely
 * 2. Excluding base pointer dependencies for field accesses
 * 3. Only including statements that "copy-propagate" values to the seed
 *
 * Key difference from traditional slicing:
 * - Traditional: x := y.f includes deps on both y (base ptr) AND o.f (field)
 * - Thin: x := y.f includes deps ONLY on o.f (the field value)
 *
 * This implementation provides:
 * - ThinSlicing: Core thin slicing algorithm (backward and forward)
 * - Context-sensitive thin slicing using CFL-reachability
 * - Hierarchical expansion for explaining aliasing
 * - Utility functions for statistics and debugging
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

/**
 * @brief Configuration options for thin slicing
 */
struct ThinSliceConfig {
  /// Whether to use context-sensitive analysis (CFL-reachability)
  bool context_sensitive = false;

  /// Maximum call stack depth for context-sensitive analysis (0 = unlimited)
  size_t max_stack_depth = 0;

  /// Maximum states to explore (0 = unlimited)
  size_t max_states = 0;

  /// Whether to include return value dependencies
  bool include_return_deps = true;

  /// Whether to include parameter dependencies (for interprocedural flow)
  bool include_parameter_deps = true;
};

/**
 * @brief Diagnostics information for thin slicing
 */
struct ThinSliceDiagnostics {
  /// Number of nodes in the thin slice
  size_t slice_size = 0;

  /// Number of nodes that would be in traditional slice (for comparison)
  size_t traditional_slice_size = 0;

  /// Number of base pointer dependencies excluded
  size_t base_ptr_deps_excluded = 0;

  /// Number of control dependencies excluded
  size_t control_deps_excluded = 0;

  /// Maximum call stack depth reached (context-sensitive only)
  size_t max_stack_depth_reached = 0;

  /// Whether state limit was hit
  bool state_limit_hit = false;

  /// Whether stack depth limit was hit
  bool stack_depth_limit_hit = false;
};

/**
 * @brief Thin slicing implementation for PDG
 *
 * Implements thin slicing algorithm that produces smaller slices by:
 * - Excluding control dependencies
 * - Tracking only value flow (not base pointer flow) for memory operations
 *
 * The key insight is that for field access x := y.f, we only track
 * dependencies that define the value stored in o.f, not dependencies
 * that define the base pointer y.
 */
class ThinSlicing {
public:
  using NodeSet = std::set<Node *>;
  using VisitedSet = std::unordered_set<Node *>;

  /**
   * @brief Constructor
   * @param pdg Reference to the program dependency graph
   */
  explicit ThinSlicing(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Compute thin backward slice from a single seed node
   *
   * Computes the thin slice by following data dependencies backward,
   * excluding control dependencies and base pointer dependencies.
   *
   * @param seed_node The seed node (slicing criterion)
   * @param config Optional configuration for the slicing
   * @param diagnostics Optional output for diagnostic information
   * @return Set of nodes in the thin slice
   */
  NodeSet computeBackwardSlice(Node &seed_node,
                               const ThinSliceConfig &config = {},
                               ThinSliceDiagnostics *diagnostics = nullptr);

  /**
   * @brief Compute thin backward slice from multiple seed nodes
   * @param seed_nodes Set of seed nodes
   * @param config Optional configuration for the slicing
   * @param diagnostics Optional output for diagnostic information
   * @return Set of nodes in the thin slice
   */
  NodeSet computeBackwardSlice(const NodeSet &seed_nodes,
                               const ThinSliceConfig &config = {},
                               ThinSliceDiagnostics *diagnostics = nullptr);

  /**
   * @brief Compute thin forward slice from a single node
   *
   * Computes nodes that may be affected by the source through value flow,
   * excluding control dependencies and base pointer propagation.
   *
   * @param source_node The source node
   * @param config Optional configuration for the slicing
   * @param diagnostics Optional output for diagnostic information
   * @return Set of nodes in the thin forward slice
   */
  NodeSet computeForwardSlice(Node &source_node,
                              const ThinSliceConfig &config = {},
                              ThinSliceDiagnostics *diagnostics = nullptr);

  /**
   * @brief Compute thin forward slice from multiple source nodes
   * @param source_nodes Set of source nodes
   * @param config Optional configuration for the slicing
   * @param diagnostics Optional output for diagnostic information
   * @return Set of nodes in the thin forward slice
   */
  NodeSet computeForwardSlice(const NodeSet &source_nodes,
                              const ThinSliceConfig &config = {},
                              ThinSliceDiagnostics *diagnostics = nullptr);

  /**
   * @brief Expand thin slice to explain aliasing
   *
   * When two field accesses x.f and y.f may alias, this computes
   * additional thin slices for the base pointers to explain how
   * they may point to the same object.
   *
   * @param slice The current thin slice
   * @param config Optional configuration
   * @return Map from base pointer nodes to their thin slices
   */
  std::unordered_map<Node *, NodeSet>
  expandForAliasing(const NodeSet &slice, const ThinSliceConfig &config = {});

  /**
   * @brief Check if an edge represents value flow (not base pointer flow)
   *
   * For thin slicing, we distinguish between:
   * - Value flow: The actual data being propagated
   * - Base pointer flow: The pointer used to access the data
   *
   * @param edge The edge to check
   * @param src_node Source node of the edge
   * @param dst_node Destination node of the edge
   * @return True if this edge represents value flow
   */
  bool isValueFlowEdge(Edge *edge, Node *src_node, Node *dst_node);

  /**
   * @brief Check if a node is a field access (load/store through pointer)
   * @param node The node to check
   * @return True if this is a field access
   */
  bool isFieldAccess(Node *node);

  /**
   * @brief Get the base pointer node for a field access
   * @param field_access_node The field access node
   * @return The base pointer node, or nullptr if not a field access
   */
  Node *getBasePointerNode(Node *field_access_node);

private:
  GenericGraph &_pdg;

  /**
   * @brief Internal backward traversal for thin slicing
   *
   * @param start_nodes Starting nodes
   * @param config Configuration options
   * @param diagnostics Optional diagnostics output
   * @return Set of nodes in the thin slice
   */
  NodeSet traverseBackward(const NodeSet &start_nodes,
                           const ThinSliceConfig &config,
                           ThinSliceDiagnostics *diagnostics);

  /**
   * @brief Internal forward traversal for thin slicing
   *
   * @param start_nodes Starting nodes
   * @param config Configuration options
   * @param diagnostics Optional diagnostics output
   * @return Set of nodes in the thin slice
   */
  NodeSet traverseForward(const NodeSet &start_nodes,
                          const ThinSliceConfig &config,
                          ThinSliceDiagnostics *diagnostics);

  /**
   * @brief Context-sensitive backward traversal using CFL-reachability
   *
   * @param start_nodes Starting nodes
   * @param config Configuration options
   * @param diagnostics Optional diagnostics output
   * @return Set of nodes in the thin slice
   */
  NodeSet traverseBackwardContextSensitive(const NodeSet &start_nodes,
                                           const ThinSliceConfig &config,
                                           ThinSliceDiagnostics *diagnostics);

  /**
   * @brief Context-sensitive forward traversal using CFL-reachability
   *
   * @param start_nodes Starting nodes
   * @param config Configuration options
   * @param diagnostics Optional diagnostics output
   * @return Set of nodes in the thin slice
   */
  NodeSet traverseForwardContextSensitive(const NodeSet &start_nodes,
                                          const ThinSliceConfig &config,
                                          ThinSliceDiagnostics *diagnostics);

  /**
   * @brief Check if an edge type is a control dependency
   * @param edge_type The edge type to check
   * @return True if this is a control dependency edge
   */
  bool isControlDependencyEdge(EdgeType edge_type);

  /**
   * @brief Check if an edge type is a data/value dependency
   * @param edge_type The edge type to check
   * @return True if this is a data dependency edge
   */
  bool isDataDependencyEdge(EdgeType edge_type);

  /**
   * @brief Get the set of edge types used for thin slicing
   * @param config Configuration options
   * @return Set of edge types to follow
   */
  std::set<EdgeType> getThinSliceEdgeTypes(const ThinSliceConfig &config);
};

/**
 * @brief Utility class for thin slicing operations
 */
class ThinSlicingUtils {
public:
  using NodeSet = std::set<Node *>;

  /**
   * @brief Get edge types for value flow (thin slicing)
   *
   * Returns the set of edge types that represent direct value flow,
   * excluding control dependencies.
   *
   * @return Set of value flow edge types
   */
  static std::set<EdgeType> getValueFlowEdgeTypes();

  /**
   * @brief Get edge types that are excluded from thin slicing
   * @return Set of excluded edge types (control deps, etc.)
   */
  static std::set<EdgeType> getExcludedEdgeTypes();

  /**
   * @brief Compare thin slice with traditional slice
   *
   * Computes statistics about the difference between thin and traditional
   * slices to quantify the precision improvement.
   *
   * @param thin_slice The thin slice
   * @param traditional_slice The traditional (full) slice
   * @return Map of comparison statistics
   */
  static std::unordered_map<std::string, size_t>
  compareWithTraditionalSlice(const NodeSet &thin_slice,
                              const NodeSet &traditional_slice);

  /**
   * @brief Print thin slice information to stderr
   * @param slice Set of nodes in the slice
   * @param slice_name Name for identification
   */
  static void printThinSlice(const NodeSet &slice,
                             const std::string &slice_name);

  /**
   * @brief Get thin slice statistics
   * @param slice Set of nodes in the slice
   * @return Map of statistics
   */
  static std::unordered_map<std::string, size_t>
  getThinSliceStatistics(const NodeSet &slice);

  /**
   * @brief Identify base pointer nodes in a slice
   *
   * For field accesses in the slice, identifies which nodes
   * represent base pointers (excluded from thin slice proper
   * but may be needed for hierarchical expansion).
   *
   * @param slice The thin slice
   * @param pdg Reference to the PDG
   * @return Set of base pointer nodes
   */
  static NodeSet identifyBasePointerNodes(const NodeSet &slice,
                                          GenericGraph &pdg);

  /**
   * @brief Check if a node represents a memory load
   * @param node The node to check
   * @return True if this is a load instruction
   */
  static bool isLoadNode(Node *node);

  /**
   * @brief Check if a node represents a memory store
   * @param node The node to check
   * @return True if this is a store instruction
   */
  static bool isStoreNode(Node *node);

  /**
   * @brief Check if a node represents a GEP (field access)
   * @param node The node to check
   * @return True if this is a GEP instruction
   */
  static bool isGEPNode(Node *node);
};

} // namespace pdg
