/*
 * Path-Aware IDE Solver
 *
 * This solver extends the standard IDE solver to explicitly track and maintain
 * the exploded supergraph (ESG), enabling path queries and path-sensitive debugging.
 *
 * Based on Phasar's PathAwareIDESolver design.
 */

#pragma once

#include "Dataflow/IFDS/IFDSFramework.h"
#include "Dataflow/IFDS/IFDSIDESolverConfig.h"
#include "Dataflow/IFDS/Solvers/IDESolver.h"

#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifds {

// ============================================================================
// ESG Edge Kind Classification
// ============================================================================

enum class ESGEdgeKind {
  Normal,        // Intra-procedural flow
  Call,          // Call edge (caller -> callee entry)
  Return,        // Return edge (callee exit -> return site)
  CallToReturn,  // Call-to-return edge (bypassing callee)
  Summary        // Summary edge (call -> return site via summary)
};

inline const char* to_string(ESGEdgeKind kind) {
  switch (kind) {
    case ESGEdgeKind::Normal: return "Normal";
    case ESGEdgeKind::Call: return "Call";
    case ESGEdgeKind::Return: return "Return";
    case ESGEdgeKind::CallToReturn: return "CallToReturn";
    case ESGEdgeKind::Summary: return "Summary";
  }
  return "Unknown";
}

// ============================================================================
// Exploded Supergraph with Edge Tracking
// ============================================================================

template<typename Fact>
class ExplicitExplodedSupergraph {
public:
  using Node = typename ExplodedSupergraph<Fact>::Node;
  using NodeHash = typename ExplodedSupergraph<Fact>::NodeHash;

  struct ESGEdge {
    Node source;
    Node target;
    ESGEdgeKind kind;

    ESGEdge(const Node& src, const Node& tgt, ESGEdgeKind k)
        : source(src), target(tgt), kind(k) {}

    bool operator==(const ESGEdge& other) const {
      return source == other.source && target == other.target && kind == other.kind;
    }
  };

  struct ESGEdgeHash {
    size_t operator()(const ESGEdge& edge) const {
      size_t h1 = NodeHash{}(edge.source);
      size_t h2 = NodeHash{}(edge.target);
      size_t h3 = std::hash<int>{}(static_cast<int>(edge.kind));
      return ((h1 ^ (h2 << 1)) ^ (h3 << 2));
    }
  };

  ExplicitExplodedSupergraph() = default;

  // Add an edge to the ESG
  void add_edge(const Node& source, const Node& target, ESGEdgeKind kind) {
    ESGEdge edge(source, target, kind);
    if (m_edges.insert(edge).second) {
      m_successors[source].push_back(edge);
      m_predecessors[target].push_back(edge);
      m_nodes.insert(source);
      m_nodes.insert(target);
    }
  }

  // Save edges from a set (batch operation)
  template<typename FactSet>
  void save_edges(const llvm::Instruction* curr_inst, const Fact& curr_fact,
                  const llvm::Instruction* succ_inst, const FactSet& succ_facts,
                  ESGEdgeKind kind) {
    Node source(curr_inst, curr_fact);
    for (const auto& succ_fact : succ_facts) {
      Node target(succ_inst, succ_fact);
      add_edge(source, target, kind);
    }
  }

  // Query interface
  std::vector<ESGEdge> get_successors(const Node& node) const {
    auto it = m_successors.find(node);
    return it != m_successors.end() ? it->second : std::vector<ESGEdge>{};
  }

  std::vector<ESGEdge> get_predecessors(const Node& node) const {
    auto it = m_predecessors.find(node);
    return it != m_predecessors.end() ? it->second : std::vector<ESGEdge>{};
  }

  const std::unordered_set<ESGEdge, ESGEdgeHash>& get_all_edges() const {
    return m_edges;
  }

  const std::unordered_set<Node, NodeHash>& get_all_nodes() const {
    return m_nodes;
  }

  size_t num_edges() const { return m_edges.size(); }
  size_t num_nodes() const { return m_nodes.size(); }

  bool has_edge(const Node& source, const Node& target, ESGEdgeKind kind) const {
    return m_edges.count(ESGEdge(source, target, kind)) > 0;
  }

  // Path finding (simple BFS)
  std::vector<std::vector<ESGEdge>> find_paths(const Node& start, const Node& end,
                                               size_t max_paths = 10,
                                               size_t max_depth = 100) const {
    std::vector<std::vector<ESGEdge>> paths;
    std::vector<ESGEdge> current_path;
    std::unordered_set<Node, NodeHash> visited;

    find_paths_dfs(start, end, current_path, visited, paths, max_paths, max_depth);
    return paths;
  }

  // Export to DOT format for visualization
  void export_to_dot(llvm::raw_ostream& os, const std::string& graph_name = "ESG") const {
    os << "digraph " << graph_name << " {\n";
    os << "  rankdir=TB;\n";
    os << "  node [shape=box];\n\n";

    // Nodes
    std::unordered_map<Node, size_t, NodeHash> node_ids;
    size_t id = 0;
    for (const auto& node : m_nodes) {
      node_ids[node] = id;
      os << "  n" << id << " [label=\"";
      if (node.instruction) {
        os << node.instruction->getParent()->getParent()->getName().str() << "\\n";
        std::string inst_str;
        llvm::raw_string_ostream rso(inst_str);
        node.instruction->print(rso);
        rso.flush();
        // Escape quotes and limit length
        for (char c : inst_str) {
          if (c == '"') os << "\\\"";
          else if (c == '\n') os << "\\n";
          else os << c;
        }
      } else {
        os << "NULL";
      }
      os << "\"];\n";
      id++;
    }

    // Edges
    os << "\n";
    for (const auto& edge : m_edges) {
      size_t src_id = node_ids[edge.source];
      size_t tgt_id = node_ids[edge.target];
      os << "  n" << src_id << " -> n" << tgt_id;
      os << " [label=\"" << to_string(edge.kind) << "\"";

      // Color edges by kind
      switch (edge.kind) {
        case ESGEdgeKind::Normal:
          os << ", color=black";
          break;
        case ESGEdgeKind::Call:
          os << ", color=blue";
          break;
        case ESGEdgeKind::Return:
          os << ", color=green";
          break;
        case ESGEdgeKind::CallToReturn:
          os << ", color=orange";
          break;
        case ESGEdgeKind::Summary:
          os << ", color=red, style=dashed";
          break;
      }
      os << "];\n";
    }

    os << "}\n";
  }

private:
  std::unordered_set<ESGEdge, ESGEdgeHash> m_edges;
  std::unordered_set<Node, NodeHash> m_nodes;
  std::unordered_map<Node, std::vector<ESGEdge>, NodeHash> m_successors;
  std::unordered_map<Node, std::vector<ESGEdge>, NodeHash> m_predecessors;

  void find_paths_dfs(const Node& current, const Node& end,
                     std::vector<ESGEdge>& current_path,
                     std::unordered_set<Node, NodeHash>& visited,
                     std::vector<std::vector<ESGEdge>>& paths,
                     size_t max_paths, size_t max_depth) const {
    if (paths.size() >= max_paths || current_path.size() >= max_depth) {
      return;
    }

    if (current == end) {
      paths.push_back(current_path);
      return;
    }

    if (!visited.insert(current).second) {
      return; // Already visited
    }

    for (const auto& edge : get_successors(current)) {
      current_path.push_back(edge);
      find_paths_dfs(edge.target, end, current_path, visited, paths, max_paths, max_depth);
      current_path.pop_back();
    }

    visited.erase(current);
  }
};

// ============================================================================
// Path-Aware IDE Solver
// ============================================================================

template<typename Problem>
class PathAwareIDESolver : public IDESolver<Problem> {
public:
  using Base = IDESolver<Problem>;
  using Fact = typename Problem::FactType;
  using Value = typename Problem::ValueType;
  using Node = typename ExplodedSupergraph<Fact>::Node;
  using NodeHash = typename ExplodedSupergraph<Fact>::NodeHash;
  using FactSet = typename Problem::FactSet;
  using ESGEdge = typename ExplicitExplodedSupergraph<Fact>::ESGEdge;

  PathAwareIDESolver(Problem& problem)
      : Base(problem), m_record_edges(true) {}

  void solve(const llvm::Module& module) {
    // Clear ESG before solving
    m_esg = ExplicitExplodedSupergraph<Fact>();
    
    // Configure to record edges
    auto& config = this->get_solver_config();
    config.set_record_edges(true);
    
    // Call base solver
    Base::solve(module);
  }

  // Access the explicit ESG
  const ExplicitExplodedSupergraph<Fact>& get_esg() const {
    return m_esg;
  }

  ExplicitExplodedSupergraph<Fact>& get_esg() {
    return m_esg;
  }

  // Path queries
  std::vector<std::vector<ESGEdge>> find_paths(
      const llvm::Instruction* start_inst, const Fact& start_fact,
      const llvm::Instruction* end_inst, const Fact& end_fact,
      size_t max_paths = 10, size_t max_depth = 100) const {
    Node start(start_inst, start_fact);
    Node end(end_inst, end_fact);
    return m_esg.find_paths(start, end, max_paths, max_depth);
  }

  // Export ESG to DOT file
  void export_esg_to_dot(const std::string& filename) const {
    std::error_code ec;
    llvm::raw_fd_ostream file(filename, ec);
    if (ec) {
      llvm::errs() << "Error opening file " << filename << ": " << ec.message() << "\n";
      return;
    }
    m_esg.export_to_dot(file);
  }

  void export_esg_to_dot(llvm::raw_ostream& os) const {
    m_esg.export_to_dot(os);
  }

  // Statistics
  size_t get_esg_node_count() const { return m_esg.num_nodes(); }
  size_t get_esg_edge_count() const { return m_esg.num_edges(); }

protected:
  // Override to record edges during solving
  void record_flow_edge(const llvm::Instruction* curr_inst, const Fact& curr_fact,
                       const llvm::Instruction* succ_inst, const FactSet& succ_facts,
                       ESGEdgeKind kind) {
    if (m_record_edges) {
      m_esg.save_edges(curr_inst, curr_fact, succ_inst, succ_facts, kind);
    }
  }

  // Hook into IDESolver's propagation
  friend class IDESolver<Problem>;

  ExplicitExplodedSupergraph<Fact> m_esg;
  bool m_record_edges;
};

} // namespace ifds
