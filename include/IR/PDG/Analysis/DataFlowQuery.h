/**
 * @file DataFlowQuery.h
 * @brief Classic data-flow analyses extracted from the PDG.
 *
 * The PDG already encodes data and control dependence information.  This file
 * provides higher-level query interfaces that frame standard data-flow
 * analyses in terms of the existing PDG representation:
 *
 * 1. **ReachingDefinitions** -- for a given use node, find all definitions
 *    (stores / assignments) that may reach it via data dependence edges.
 *    (Kuck, Kuhn, Padua, Leasure & Wolfe, 1981; Kennedy, 1978)
 *
 * 2. **DefUseChains** -- enumerate all def-use and use-def chains reachable
 *    from a given node in the PDG, optionally with field sensitivity.
 *
 * 3. **LiveVariables** -- approximate liveness by inspecting which PDG nodes
 *    have outgoing data-dependence edges (a node is "live" if its value is
 *    used later).
 *
 * 4. **DataOnlySlicing** -- convenience wrappers that restrict forward /
 *    backward slicing to *only* data dependence edges (excluding control
 *    dependence).  This is a common need in taint analysis and information
 *    flow analysis (Denning & Denning, 1977).
 *
 * 5. **ControlDependenceQuery** -- identify the controlling conditions for a
 *    given node (or set of nodes) by walking control dependence edges
 *    backward.  Standard in predicate-based PDG analyses (Ferrante et al. '87).
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
// ReachingDefinitions
// ============================================================================

/**
 * @brief A reaching definition: a (defining node, edge type) pair.
 *
 * Represents a single definition that reaches a use, along with the type
 * of data-dependence edge connecting them. Useful for understanding the
 * specific relationship (def-use, alias, parameter passing, etc.).
 *
 * @see ReachingDefinitions::directDefs() for obtaining these
 */
struct ReachingDef {
  Node *def_node = nullptr;
  EdgeType edge_type = EdgeType::DATA_DEF_USE;
};

/**
 * @brief Reaching definitions query over the PDG.
 *
 * For a given "use" node, enumerates all definition nodes that are connected
 * to it via data-dependence edges (directly or transitively).  The PDG
 * already stores def-use edges; this class provides a cleaner query API with
 * filtering and depth-limiting options.
 *
 * **Classic use cases:**
 * - **Data-flow analysis**: "What definitions can reach this use?"
 * - **Taint analysis**: "Where did this tainted value originate?"
 * - **Debugging**: "What assignments contributed to this value?"
 * - **Optimization**: Finding dead stores (definitions that don't reach any use)
 *
 * **Relationship to PDG:**
 * The PDG already encodes def-use relationships via DATA_DEF_USE, DATA_RAW,
 * and other data-dependence edges. This class provides a convenient query
 * interface that abstracts away the edge-type details.
 *
 * @see pdg::DefUseChains for enumerating complete def-use chains
 * @see pdg::DataOnlySlicing for full backward slices restricted to data edges
 * @see pdg::Slicing for full backward slices including control dependencies
 *
 * Example usage:
 * @code
 *   ReachingDefinitions rd(pdg);
 *   // Find all definitions reaching a use
 *   auto defs = rd.transitiveDefs(use_node);
 *   for (Node *def : defs) {
 *     errs() << "Definition reaches use: " << def << "\n";
 *   }
 *   // Find all uses of a definition
 *   auto uses = rd.transitiveUses(def_node);
 * @endcode
 */
class ReachingDefinitions {
public:
  using NodeSet = std::set<Node *>;

  /**
   * @brief Constructor
   * @param pdg Reference to the program dependency graph
   */
  explicit ReachingDefinitions(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Find all definitions reaching a use node (single hop).
   *
   * Returns only *direct* (single-edge) definitions -- i.e. the immediate
   * data-dependence predecessors.
   *
   * @param use_node The use node
   * @return Vector of reaching definitions
   */
  std::vector<ReachingDef> directDefs(Node &use_node);

  /**
   * @brief Find all definitions reaching a use node (transitive).
   *
   * Follows data-dependence edges backward transitively until no more
   * definitions are found.  Useful for finding the *origin* of a value
   * across copies, phis, and parameter passing.
   *
   * **Algorithm:** Performs a backward BFS from @p use_node following
   * data-dependence edges. Time complexity is O(V+E) in the worst case.
   *
   * **Edge types followed:** DATA_DEF_USE, DATA_RAW, DATA_READ, DATA_ALIAS,
   * DATA_RET, VAL_DEP, PARAMETER_IN, PARAMETER_OUT, PARAMETER_FIELD
   *
   * @param use_node  The use node (destination of the backward traversal)
   * @param max_depth Depth limit in edges (0 = unlimited). Use this to
   *                  bound analysis time on large graphs.
   * @return Set of all definition nodes that can reach @p use_node
   *
   * @note This is equivalent to a backward data-only slice from @p use_node.
   *       See DataOnlySlicing::backwardSlice() for the full slice API.
   */
  NodeSet transitiveDefs(Node &use_node, size_t max_depth = 0);

  /**
   * @brief Find all uses of a definition node (single hop).
   *
   * Returns the set of nodes that directly use the value defined at
   * @p def_node.
   *
   * @param def_node The definition node
   * @return Set of use nodes
   */
  NodeSet directUses(Node &def_node);

  /**
   * @brief Find all transitive uses of a definition.
   * @param def_node  The definition node
   * @param max_depth Depth limit (0 = unlimited)
   * @return Set of all use nodes
   */
  NodeSet transitiveUses(Node &def_node, size_t max_depth = 0);

private:
  GenericGraph &_pdg;

  /// Data-dependence edge types used by the analysis.
  static std::set<EdgeType> getDataEdgeTypes();
};

// ============================================================================
// DefUseChains
// ============================================================================

/**
 * @brief A link in a def-use or use-def chain.
 *
 * Represents a single hop in a data-flow chain, connecting two nodes via
 * a specific edge type. Chains are sequences of these links showing how
 * values flow through the program.
 *
 * @see DefUseChains for building complete chains
 */
struct DefUseLink {
  Node *from = nullptr;
  Node *to = nullptr;
  EdgeType edge_type = EdgeType::DATA_DEF_USE;
};

/**
 * @brief Def-use and use-def chain enumeration.
 *
 * Provides the classic def-use chain abstraction from compiler textbooks
 * (Aho, Sethi, Ullman) but extracted from the PDG rather than computed
 * from scratch. A def-use chain is a sequence of nodes connected by
 * data-dependence edges, representing how values flow through the program.
 *
 * **Use cases:**
 * - **Value-flow analysis**: Trace how a value propagates through the program
 * - **Program understanding**: Visualize data-flow paths
 * - **Optimization**: Identify opportunities for copy propagation, constant
 *   folding, etc.
 * - **Debugging**: Understand the complete path a value takes
 *
 * **Difference from ReachingDefinitions:**
 * - ReachingDefinitions returns *sets* of definitions/uses
 * - DefUseChains returns *ordered sequences* showing the flow path
 *
 * @see pdg::ReachingDefinitions for set-based queries
 * @see pdg::DataOnlySlicing for full slices
 *
 * Example usage:
 * @code
 *   DefUseChains chains(pdg);
 *   // Get the def-use chain from a definition
 *   auto chain = chains.getDefUseChain(def_node);
 *   for (const auto &link : chain) {
 *     errs() << link.from << " -> " << link.to
 *            << " via " << link.edge_type << "\n";
 *   }
 * @endcode
 */
class DefUseChains {
public:
  using NodeSet = std::set<Node *>;
  using Chain = std::vector<DefUseLink>;

  /**
   * @brief Constructor
   * @param pdg Reference to the program dependency graph
   */
  explicit DefUseChains(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Get the def-use chain originating at @p def_node.
   *
   * Returns a chain of DefUseLink entries following data edges forward.
   * Each link represents one hop in the chain.
   *
   * @param def_node  Starting definition node
   * @param max_depth Chain length limit (0 = unlimited)
   * @return The def-use chain
   */
  Chain getDefUseChain(Node &def_node, size_t max_depth = 0);

  /**
   * @brief Get the use-def chain ending at @p use_node.
   *
   * Follows data edges backward to find the chain of definitions that
   * contribute to the value at @p use_node.
   *
   * @param use_node  Starting use node
   * @param max_depth Chain length limit (0 = unlimited)
   * @return The use-def chain (in backward order)
   */
  Chain getUseDefChain(Node &use_node, size_t max_depth = 0);

  /**
   * @brief Build all def-use chains in a sub-graph.
   *
   * For each definition node in @p nodes, computes its def-use chain.
   *
   * @param nodes     Sub-graph to analyze
   * @param max_depth Per-chain depth limit (0 = unlimited)
   * @return Map from definition node to its def-use chain
   */
  std::unordered_map<Node *, Chain>
  allDefUseChains(const NodeSet &nodes, size_t max_depth = 0);

private:
  GenericGraph &_pdg;
};

// ============================================================================
// LiveVariables (approximation via PDG)
// ============================================================================

/**
 * @brief Approximate live-variable analysis from the PDG.
 *
 * A node is considered "live" if its value is used by at least one successor
 * via a data-dependence edge.  This is an approximation of classic liveness
 * (Aho, Sethi, Ullman) derived from the already-computed PDG.
 *
 * **Approximation note:** Classic liveness analysis considers control flow
 * (a variable is live if there exists a path to a use without redefinition).
 * This PDG-based approximation considers only data-dependence edges, which
 * may miss some liveness relationships but is faster and often sufficient.
 *
 * **Use cases:**
 * - **Dead code elimination**: Identify nodes with no outgoing data edges
 * - **Register allocation**: Understand which values need to be kept in registers
 * - **Program understanding**: See which computations produce unused results
 *
 * **Limitations:**
 * - May classify some live variables as dead (if control flow matters)
 * - Does not consider control dependencies
 * - Sub-graph analysis may miss uses outside the sub-graph
 *
 * @see pdg::ReachingDefinitions for finding where values are used
 * @see pdg::DataOnlySlicing for full forward slices
 *
 * Example usage:
 * @code
 *   LiveVariables liveness(pdg);
 *   if (liveness.isLive(node)) {
 *     errs() << "Node is live\n";
 *   }
 *   auto dead = liveness.deadNodesIn(subgraph);
 *   errs() << "Dead nodes: " << dead.size() << "\n";
 * @endcode
 */
class LiveVariables {
public:
  using NodeSet = std::set<Node *>;

  /**
   * @brief Constructor
   * @param pdg Reference to the program dependency graph
   */
  explicit LiveVariables(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Check if a node is live (has data-dep successors).
   * @param node The node to check
   * @return True if the node's value is used
   */
  bool isLive(Node &node);

  /**
   * @brief Get all live nodes in the graph.
   * @return Set of live nodes
   */
  NodeSet allLiveNodes();

  /**
   * @brief Get all live nodes in a sub-graph.
   * @param subgraph Nodes to consider
   * @return Set of live nodes within the sub-graph
   */
  NodeSet liveNodesIn(const NodeSet &subgraph);

  /**
   * @brief Get all dead (non-live) nodes in a sub-graph.
   *
   * Dead nodes have no outgoing data dependence edges within the sub-graph.
   * These may represent dead stores or unused computations.
   *
   * @param subgraph Nodes to consider
   * @return Set of dead nodes
   */
  NodeSet deadNodesIn(const NodeSet &subgraph);

private:
  GenericGraph &_pdg;

  /// Data-dependence edge types considered for liveness.
  static std::set<EdgeType> getDataEdgeTypes();
};

// ============================================================================
// DataOnlySlicing
// ============================================================================

/**
 * @brief Forward and backward slicing restricted to data dependence edges.
 *
 * Convenience class for a common analysis pattern: compute slices that
 * ignore control dependence, focusing purely on data flow.  This is widely
 * used in taint/information-flow analysis (Denning & Denning, 1977).
 *
 * **Key difference from pdg::Slicing:**
 * - pdg::Slicing includes both data AND control dependencies
 * - DataOnlySlicing includes ONLY data dependencies
 *
 * **Use cases:**
 * - **Taint analysis**: Track how tainted data flows through the program
 * - **Information flow**: Understand where sensitive data propagates
 * - **Value-flow debugging**: Focus on data dependencies, ignore control
 * - **Optimization**: Identify data-only slices for parallelization
 *
 * **When to use vs. pdg::Slicing:**
 * - Use DataOnlySlicing when control flow is irrelevant (e.g., taint analysis)
 * - Use pdg::Slicing when you need the full program slice including predicates
 *
 * @see pdg::Slicing for full slices (data + control dependencies)
 * @see pdg::ThinSlicing for even more precise value-flow slices
 * @see pdg::ReachingDefinitions for backward queries from a single use
 *
 * Example usage:
 * @code
 *   DataOnlySlicing dos(pdg);
 *   // Forward slice: what nodes are affected by this data?
 *   auto forward = dos.forwardSlice(source_node);
 *   // Backward slice: where did this data come from?
 *   auto backward = dos.backwardSlice(sink_node);
 * @endcode
 */
class DataOnlySlicing {
public:
  using NodeSet = std::set<Node *>;

  /**
   * @brief Constructor
   * @param pdg Reference to the program dependency graph
   */
  explicit DataOnlySlicing(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Forward data-only slice from a single node.
   * @param start_node Starting node
   * @param max_depth  Depth limit (0 = unlimited)
   * @return Set of nodes in the data-only forward slice
   */
  NodeSet forwardSlice(Node &start_node, size_t max_depth = 0);

  /**
   * @brief Forward data-only slice from multiple nodes.
   * @param start_nodes Starting nodes
   * @param max_depth   Depth limit (0 = unlimited)
   * @return Set of nodes in the data-only forward slice
   */
  NodeSet forwardSlice(const NodeSet &start_nodes, size_t max_depth = 0);

  /**
   * @brief Backward data-only slice from a single node.
   * @param end_node Ending node
   * @param max_depth Depth limit (0 = unlimited)
   * @return Set of nodes in the data-only backward slice
   */
  NodeSet backwardSlice(Node &end_node, size_t max_depth = 0);

  /**
   * @brief Backward data-only slice from multiple nodes.
   * @param end_nodes Ending nodes
   * @param max_depth Depth limit (0 = unlimited)
   * @return Set of nodes in the data-only backward slice
   */
  NodeSet backwardSlice(const NodeSet &end_nodes, size_t max_depth = 0);

  /**
   * @brief Get the set of data-dependence edge types used.
   * @return Set of edge types
   */
  static std::set<EdgeType> getDataEdgeTypes();

private:
  GenericGraph &_pdg;
};

// ============================================================================
// ControlDependenceQuery
// ============================================================================

/**
 * @brief A controlling condition: a predicate node and the branch type.
 *
 * Represents a single predicate that controls the execution of a node,
 * along with the type of control-dependence edge (branch, call, entry, etc.).
 *
 * @see ControlDependenceQuery::immediateControllers() for obtaining these
 */
struct ControllingCondition {
  Node *predicate = nullptr;
  EdgeType edge_type = EdgeType::CONTROLDEP_BR;
};

/**
 * @brief Queries for control-dependence relationships in the PDG.
 *
 * Identifies the controlling conditions (predicates) for a given node by
 * following control-dependence edges backward.  This implements the standard
 * "what controls this statement?" query from Ferrante et al. (1987).
 *
 * **Control dependence:** A node B is control-dependent on node A if:
 * - A is a predicate (branch, call, etc.)
 * - The execution of B depends on the outcome of A
 * - There exists a path from A to B where B executes, and another path
 *   where B does not execute
 *
 * **Use cases:**
 * - **Program understanding**: "What conditions must be true for this code to execute?"
 * - **Test generation**: Identify predicates that control a target statement
 * - **Debugging**: Understand the control-flow context of a bug
 * - **Refactoring**: Identify control-dependence regions for restructuring
 *
 * **Edge types considered:** CONTROLDEP_CALLINV, CONTROLDEP_CALLRET,
 * CONTROLDEP_ENTRY, CONTROLDEP_BR, CONTROLDEP_IND_BR
 *
 * @see pdg::Slicing for slices that include control dependencies
 * @see pdg::DataOnlySlicing for slices that exclude control dependencies
 *
 * Example usage:
 * @code
 *   ControlDependenceQuery ctrl(pdg);
 *   // Find what controls this statement
 *   auto controllers = ctrl.allControllers(target_node);
 *   for (Node *pred : controllers) {
 *     errs() << "Controlled by: " << pred << "\n";
 *   }
 *   // Find what this predicate controls
 *   auto controlled = ctrl.controlRegion(predicate_node);
 *   errs() << "Controls " << controlled.size() << " nodes\n";
 * @endcode
 */
class ControlDependenceQuery {
public:
  using NodeSet = std::set<Node *>;

  /**
   * @brief Constructor
   * @param pdg Reference to the program dependency graph
   */
  explicit ControlDependenceQuery(GenericGraph &pdg) : _pdg(pdg) {}

  /**
   * @brief Find the immediate controlling conditions for a node.
   *
   * Returns the set of predicate nodes that directly control whether
   * @p node executes (i.e. its control-dependence predecessors).
   *
   * @param node The node to query
   * @return Vector of controlling conditions
   */
  std::vector<ControllingCondition> immediateControllers(Node &node);

  /**
   * @brief Find all transitive controlling conditions.
   *
   * Follows control-dependence edges backward transitively to find all
   * predicates in the nesting hierarchy that govern execution of @p node.
   *
   * @param node The node to query
   * @param max_depth Depth limit (0 = unlimited)
   * @return Set of all controlling predicate nodes
   */
  NodeSet allControllers(Node &node, size_t max_depth = 0);

  /**
   * @brief Find nodes that are control-dependent on a given predicate.
   *
   * Returns all nodes whose execution is controlled by @p predicate_node
   * (i.e. control-dependence successors).
   *
   * @param predicate_node The predicate node
   * @return Set of control-dependent nodes
   */
  NodeSet controlledBy(Node &predicate_node);

  /**
   * @brief Compute the control-dependence region for a predicate.
   *
   * The control-dependence region of a predicate P is the set of all nodes
   * transitively control-dependent on P.
   *
   * @param predicate_node The predicate node
   * @param max_depth      Depth limit (0 = unlimited)
   * @return Set of nodes in the control-dependence region
   */
  NodeSet controlRegion(Node &predicate_node, size_t max_depth = 0);

  /**
   * @brief Get the nesting depth of a node in the control-dependence hierarchy.
   *
   * The nesting depth is the number of control-dependence edges on the longest
   * backward path to a FUNC_ENTRY node. This measures how deeply nested a
   * statement is within control structures (if-statements, loops, etc.).
   *
   * **Use cases:**
   * - **Complexity metrics**: Higher nesting depth = more complex control flow
   * - **Refactoring**: Identify deeply nested code that might benefit from
   *   extraction
   * - **Program understanding**: Visualize control-flow hierarchy
   *
   * @param node The node to query
   * @return Nesting depth (0 for entry nodes, increases with nesting level)
   *
   * Example:
   * @code
   *   size_t depth = ctrl.nestingDepth(node);
   *   if (depth > 5) {
   *     errs() << "Deeply nested: " << depth << " levels\n";
   *   }
   * @endcode
   */
  size_t nestingDepth(Node &node);

private:
  GenericGraph &_pdg;

  /// Control-dependence edge types.
  static std::set<EdgeType> getControlEdgeTypes();
};

} // namespace pdg
