/**
 * @file QueryCore.h
 * @brief Shared types and core PDG query services.
 *
 * This header contains the shared result/configuration vocabulary used by the
 * PDG query layer plus criteria resolution helpers. Concrete query services are
 * declared in focused ``*Query`` headers to keep the public API aligned with
 * the main types that clients instantiate.
 */

#pragma once

#include "IR/PDG/Analysis/PropertySpec.h"
#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/PDGNode.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdg {

class CypherResult;

/// Named dependence-edge presets exposed to users of the query layer.
enum class PDGEdgePreset {
  All,
  Data,
  Control,
  Parameter,
  Interprocedural,
  ValueFlow,
  TransformLegality
};

/// Traversal mode for call/return matching.
enum class PDGContextMode { ContextInsensitive, ContextSensitive };

enum class PDGCachePolicy { Enabled, Disabled };

enum class SliceFlavor { Full, Thin };

enum class PDGWitnessPathKind {
  Slice,
  ShortestPath,
  AllShortestPath,
  Chop,
  TransformBlocker
};

/// Shared traversal guardrails. Zero means "unbounded".
struct PDGTraversalLimits {
  size_t max_depth = 0;
  size_t max_states = 0;
  size_t max_paths = 0;
  size_t max_path_length = 0;
  size_t max_stack_depth = 0;
};

struct PDGSourceLocation {
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
};

/// Diagnostic counters and truncation markers emitted by query execution.
struct PDGQueryDiagnostics {
  bool depth_limit_hit = false;
  bool state_limit_hit = false;
  bool path_limit_hit = false;
  bool path_length_limit_hit = false;
  bool stack_depth_limit_hit = false;
  size_t explored_states = 0;
  size_t max_depth_reached = 0;
  size_t max_stack_depth_reached = 0;
  size_t summary_cache_hits = 0;
  size_t summary_cache_misses = 0;
  size_t closure_cache_hits = 0;
  size_t closure_cache_misses = 0;
  size_t criteria_cache_hits = 0;
  size_t criteria_cache_misses = 0;
  std::vector<std::string> unresolved_criteria;
  std::vector<std::string> notes;
};

/// A concrete explanatory path returned by a query.
struct PDGWitnessPath {
  PDGWitnessPathKind kind = PDGWitnessPathKind::Slice;
  std::vector<Node *> nodes;
  std::vector<EdgeType> edge_types;
  std::vector<Node *> call_stack;
};

/// Common carrier for graph-shaped query results.
struct PDGQueryResult {
  using NodeSet = std::set<Node *>;
  using EdgeSet = std::set<Edge *>;
  using PredecessorMap = std::unordered_map<Node *, std::set<Node *>>;
  using DistanceMap = std::unordered_map<Node *, size_t>;

  NodeSet criteria_nodes;
  NodeSet nodes;
  EdgeSet edges;
  PredecessorMap predecessors;
  DistanceMap distances;
  std::vector<PDGWitnessPath> witness_paths;
  PDGQueryDiagnostics diagnostics;

  bool empty() const { return nodes.empty() && edges.empty(); }
};

/// Scope restriction applied before or during traversal.
struct PDGQueryScope {
  enum class Kind { WholeGraph, NodeSet, Function, QueryResult };

  Kind kind = Kind::WholeGraph;
  PDGQueryResult::NodeSet nodes;
  const llvm::Function *function = nullptr;
  const PDGQueryResult *query_result = nullptr;

  static PDGQueryScope wholeGraph() { return PDGQueryScope{}; }

  static PDGQueryScope nodeSet(const PDGQueryResult::NodeSet &value) {
    PDGQueryScope scope;
    scope.kind = Kind::NodeSet;
    scope.nodes = value;
    return scope;
  }

  static PDGQueryScope functionScope(const llvm::Function &value) {
    PDGQueryScope scope;
    scope.kind = Kind::Function;
    scope.function = &value;
    return scope;
  }

  static PDGQueryScope queryResultScope(const PDGQueryResult &value) {
    PDGQueryScope scope;
    scope.kind = Kind::QueryResult;
    scope.query_result = &value;
    return scope;
  }
};

struct CypherSelection {
  std::string query;
  std::string binding;
};

/// Seed specification accepted by the query layer.
struct PDGCriteria {
  using NodeSet = PDGQueryResult::NodeSet;

  NodeSet nodes;
  std::vector<llvm::Value *> values;
  std::vector<std::string> function_names;
  std::vector<std::string> callee_names;
  std::vector<PDGSourceLocation> source_locations;
  std::vector<PropertySpec> property_specs;
  std::vector<CypherSelection> cypher_selections;

  bool empty() const {
    return nodes.empty() && values.empty() && function_names.empty() &&
           callee_names.empty() && source_locations.empty() &&
           property_specs.empty() && cypher_selections.empty();
  }
};

/// Execution options shared by all PDG query services.
struct PDGQueryOptions {
  PDGEdgePreset edge_preset = PDGEdgePreset::All;
  PDGQueryScope scope = PDGQueryScope::wholeGraph();
  PDGContextMode context_mode = PDGContextMode::ContextInsensitive;
  PDGTraversalLimits limits;
  PDGCachePolicy cache_policy = PDGCachePolicy::Enabled;
  bool explain = true;
  SliceFlavor slice_flavor = SliceFlavor::Full;
};

enum class ResourceKind {
  Unknown,
  Heap,
  File,
  FileDescriptor,
  Directory
};

/// Resolves high-level criteria into concrete PDG seed nodes.
class PDGCriteriaResolver {
public:
  explicit PDGCriteriaResolver(ProgramGraph &pdg) : pdg_(pdg) {}

  PDGQueryResult resolve(const PDGCriteria &criteria,
                         const PDGQueryOptions &options,
                         const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
};

struct DefUseLink {
  Node *from = nullptr;
  Node *to = nullptr;
  EdgeType edge_type = EdgeType::DATA_DEF_USE;
};

struct ControllingCondition {
  Node *predicate = nullptr;
  EdgeType edge_type = EdgeType::CONTROLDEP_BR;
};

/// Optional LLVM analyses used by transform-oriented PDG queries.
struct LLVMQueryContext {
  llvm::Function *function = nullptr;
  llvm::DominatorTree *dominator_tree = nullptr;
  llvm::PostDominatorTree *post_dominator_tree = nullptr;
  llvm::LoopInfo *loop_info = nullptr;
  llvm::MemorySSA *memory_ssa = nullptr;
};

struct MotionCheckResult {
  bool legal = false;
  Node *moving_node = nullptr;
  Node *anchor_node = nullptr;
  std::string reason;
  std::vector<Node *> blocking_path;
  std::vector<EdgeType> blocking_edge_types;
  PDGQueryDiagnostics diagnostics;
};

struct IndependenceCheckResult {
  bool independent = false;
  std::vector<Node *> witness_path_ab;
  std::vector<EdgeType> witness_edge_types_ab;
  std::vector<Node *> witness_path_ba;
  std::vector<EdgeType> witness_edge_types_ba;
  PDGQueryDiagnostics diagnostics;
};

enum class DiffKind { Added, Removed, Preserved };

enum class NodeMatchStrategy { PointerIdentity, CanonicalSource };

struct NodeDiffEntry {
  Node *node = nullptr;
  DiffKind kind = DiffKind::Preserved;
};

struct EdgeDiffEntry {
  Edge *edge = nullptr;
  DiffKind kind = DiffKind::Preserved;
};

struct DiffImpactSummary {
  std::unordered_map<std::string, size_t> functions;
  std::unordered_map<std::string, size_t> source_locations;
};

std::set<EdgeType> edgeTypesForPreset(PDGEdgePreset preset);

std::string describeNode(Node *node);

std::string stableNodeKey(Node *node);

std::string resourceKindName(ResourceKind kind);

} // namespace pdg
