#pragma once
#include "IR/PDG/Core/PDGNode.h"
#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pdg
{
  /**
   * @brief Hash function for (Node*, call_stack) pairs used in visited set
   */
  struct NodeStackHash {
    size_t operator()(const std::pair<Node *, std::vector<Node *>> &p) const {
      size_t hash = std::hash<Node *>{}(p.first);
      for (auto *node : p.second) {
        hash ^= std::hash<Node *>{}(node) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
      }
      return hash;
    }
  };

  struct NodePairHash {
    size_t operator()(const std::pair<Node *, Node *> &p) const {
      return std::hash<Node *>{}(p.first) ^ (std::hash<Node *>{}(p.second) << 1);
    }
  };

  /**
   * @brief Data and control dependence options for context-sensitive slicing.
   *
   * Mirrors the declarative options used in tabulation-based slicers (e.g. WALA):
   * choose which edges contribute to the slice. Empty edge_types means "all
   * allowed types"; use getEdgeTypes() to resolve presets or custom sets.
   */
  struct SliceOptions {
    /// Include data dependence edges (DEF_USE, RAW, READ, ALIAS, RET, etc.).
    bool include_data_deps = true;
    /// Include control dependence edges (BR, CALLINV, CALLRET, ENTRY).
    bool include_control_deps = true;
    /// Include parameter/return edges (PARAMETER_IN, PARAMETER_OUT, DATA_RET).
    bool include_param_edges = true;
    /// Include call-invocation and return edges (required for context sensitivity).
    bool include_call_return_edges = true;
    /// Optional traversal limits (0 means unlimited).
    size_t max_states = 0;
    size_t max_stack_depth = 0;
    /// Use summary cache (tabulation-style): reuse procedure summaries for same (entry, call_site).
    bool use_summary_cache = true;

    /// Build the set of edge types allowed by current options.
    std::set<EdgeType> getEdgeTypes() const;
  };

  /**
   * @brief Context-sensitive slicing using CFL-reachability (tabulation-style).
   *
   * Implements context-sensitive slicing by tabulation over the PDG: valid paths
   * are those with properly matched call/return pairs (CFL-reachability). Optional
   * summary caching avoids re-exploring the same procedure for the same caller
   * context, matching the "summary edges at callee" design used in IFDS-based
   * slicers.
   */
  class ContextSensitiveSlicing
  {
  public:
    using NodeSet = std::set<Node *>;
    using VisitedSet = std::unordered_set<std::pair<Node *, std::vector<Node *>>, NodeStackHash>;
    struct CFLTraversalLimits {
      size_t max_states = 0;
      size_t max_stack_depth = 0;
    };
    struct CFLDiagnostics {
      bool state_limit_hit = false;
      bool stack_depth_limit_hit = false;
      size_t states_explored = 0;
      size_t max_stack_depth_reached = 0;
      size_t summary_hits = 0;
      size_t summary_misses = 0;
    };
    
    /**
     * @brief Constructor
     * @param pdg Reference to the program dependency graph
     */
    explicit ContextSensitiveSlicing(GenericGraph &pdg) : _pdg(pdg) {}
    
    /**
     * @brief Compute context-sensitive forward slice from a single node
     * @param start_node The starting node for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive forward slice
     */
    NodeSet computeForwardSlice(Node &start_node, const std::set<EdgeType> &edge_types = {});

    /**
     * @brief Compute context-sensitive forward slice with traversal limits
     * @param start_node The starting node for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @param limits Optional traversal limits (0 means unlimited)
     * @param diagnostics Optional diagnostic output for truncation guardrails
     * @return Set of nodes in the context-sensitive forward slice
     */
    NodeSet computeForwardSlice(Node &start_node, const std::set<EdgeType> &edge_types,
                                const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics);
    
    /**
     * @brief Compute context-sensitive forward slice from multiple nodes
     * @param start_nodes Set of starting nodes for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive forward slice
     */
    NodeSet computeForwardSlice(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types = {});

    /**
     * @brief Compute context-sensitive forward slice from multiple nodes with traversal limits
     * @param start_nodes Set of starting nodes for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @param limits Optional traversal limits (0 means unlimited)
     * @param diagnostics Optional diagnostic output for truncation guardrails
     * @return Set of nodes in the context-sensitive forward slice
     */
    NodeSet computeForwardSlice(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types,
                                const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics);
    
    /**
     * @brief Compute context-sensitive backward slice from a single node
     * @param end_node The ending node for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive backward slice
     */
    NodeSet computeBackwardSlice(Node &end_node, const std::set<EdgeType> &edge_types = {});

    /**
     * @brief Compute context-sensitive backward slice with traversal limits
     * @param end_node The ending node for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @param limits Optional traversal limits (0 means unlimited)
     * @param diagnostics Optional diagnostic output for truncation guardrails
     * @return Set of nodes in the context-sensitive backward slice
     */
    NodeSet computeBackwardSlice(Node &end_node, const std::set<EdgeType> &edge_types,
                                 const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics);
    
    /**
     * @brief Compute context-sensitive backward slice from multiple nodes
     * @param end_nodes Set of ending nodes for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive backward slice
     */
    NodeSet computeBackwardSlice(const NodeSet &end_nodes, const std::set<EdgeType> &edge_types = {});

    /**
     * @brief Compute context-sensitive backward slice from multiple nodes with traversal limits
     * @param end_nodes Set of ending nodes for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @param limits Optional traversal limits (0 means unlimited)
     * @param diagnostics Optional diagnostic output for truncation guardrails
     * @return Set of nodes in the context-sensitive backward slice
     */
    NodeSet computeBackwardSlice(const NodeSet &end_nodes, const std::set<EdgeType> &edge_types,
                                 const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics);

    /**
     * @brief Compute context-sensitive forward slice using options (preferred API).
     */
    NodeSet computeForwardSlice(const NodeSet &start_nodes, const SliceOptions &options,
                                CFLDiagnostics *diagnostics = nullptr);

    /**
     * @brief Compute context-sensitive backward slice using options (preferred API).
     */
    NodeSet computeBackwardSlice(const NodeSet &end_nodes, const SliceOptions &options,
                                 CFLDiagnostics *diagnostics = nullptr);
    
    /**
     * @brief Compute context-sensitive chop between source and sink nodes
     * @param source_node Source node
     * @param sink_node Sink node
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive chop
     */
    NodeSet computeChop(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Check if there exists a context-sensitive path from source to sink
     * @param source_node Source node
     * @param sink_node Sink node
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return True if a context-sensitive path exists
     */
    bool hasContextSensitivePath(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types = {});
    
  private:
    GenericGraph &_pdg;

    /// Summary for one (callee-entry, call_site): nodes reachable within callee and whether we return to caller.
    struct ProcedureSummary {
      NodeSet reachable;
      bool returns_to_caller = false;
    };
    using SummaryCache = std::unordered_map<std::pair<Node *, Node *>, ProcedureSummary, NodePairHash>;

    /**
     * @brief Context-sensitive traversal with call stack (unified forward/backward).
     * When summary_cache is non-null and use_summary_cache is true, reuses procedure summaries.
     */
    NodeSet traverseWithStack(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types, bool forward);

    NodeSet traverseWithStack(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types, bool forward,
                              const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics);

    NodeSet traverseWithStack(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types, bool forward,
                              const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics,
                              bool use_summary_cache, SummaryCache *summary_cache);

    /// Compute and cache procedure summary from (entry_node, call_site); used when summary cache is enabled.
    ProcedureSummary computeProcedureSummary(Node *entry_node, Node *call_site,
                                             const std::set<EdgeType> &edge_types, bool forward);
  };

  /**
   * @brief Utility class for context-sensitive slicing operations
   */
  class ContextSensitiveSlicingUtils
  {
  public:
    using NodeSet = std::set<Node *>;
    
    /**
     * @brief Get all call/return edges for CFL-reachability
     * @return Set of call/return edge types
     */
    static std::set<EdgeType> getCallReturnEdges();
    
    /**
     * @brief Compare context-sensitive slice with context-insensitive slice
     * @param cs_slice Context-sensitive slice
     * @param ci_slice Context-insensitive slice
     * @return Statistics about the difference between slices
     */
    static std::unordered_map<std::string, size_t> compareSlices(const NodeSet &cs_slice, const NodeSet &ci_slice);
    
    /**
     * @brief Print context-sensitive slice information to stderr
     * @param slice Set of nodes in the slice
     * @param slice_name Name of the slice for identification
     */
    static void printContextSensitiveSlice(const NodeSet &slice, const std::string &slice_name);
    
    /**
     * @brief Get context-sensitive slice statistics
     * @param slice Set of nodes in the slice
     * @return Map of statistics (node types, edge types, context information, etc.)
     */
    static std::unordered_map<std::string, size_t> getContextSensitiveSliceStatistics(const NodeSet &slice);
    
    /**
     * @brief Get CFL-reachability statistics
     * @param slice Set of nodes in the slice
     * @return Map of CFL-specific statistics (call stack depths, matched pairs, etc.)
     */
    static std::unordered_map<std::string, size_t> getCFLReachabilityStatistics(const NodeSet &slice);
    
    /**
     * @brief Check if a path is CFL-valid (properly matched call/return pairs)
     * @param path Vector of nodes representing a path
     * @param pdg Reference to the program dependency graph
     * @return True if the path follows CFL-reachability constraints
     */
    static bool isCFLValidPath(const std::vector<Node *> &path, GenericGraph &pdg);
  };

} // namespace pdg
