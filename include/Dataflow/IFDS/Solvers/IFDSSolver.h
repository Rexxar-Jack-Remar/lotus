/*
 * IFDS Solver (Sequential)
 *
 * This header provides the sequential IFDS tabulation solver.
 */

#pragma once

#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/IFDS/IFDSFramework.h"
#include "Dataflow/IFDS/IFDSIDESolverConfig.h"

#include <utility>
#include <vector>

namespace ifds {

template <typename T, typename U> struct PairHash {
  size_t operator()(const std::pair<T, U> &p) const {
    return std::hash<T>{}(p.first) ^ (std::hash<U>{}(p.second) << 1);
  }
};

// ============================================================================
// IFDS Solver (Sequential)
// ============================================================================

template <typename Problem> class IFDSSolver {
public:
  using Fact = typename Problem::FactType;
  using FactSet = typename Problem::FactSet;
  using Node = typename ExplodedSupergraph<Fact>::Node;
  using NodeHash = typename ExplodedSupergraph<Fact>::NodeHash;

  IFDSSolver(Problem &problem);

  void solve(const llvm::Module &module);

  // Enable/disable progress bar display during analysis
  void set_show_progress(bool show) { m_show_progress = show; }

  // Solver configuration (return sites, unbalanced returns, etc.)
  void set_solver_config(IFDSIDESolverConfig config) {
    m_config = std::move(config);
  }
  IFDSIDESolverConfig &get_solver_config() { return m_config; }
  const IFDSIDESolverConfig &get_solver_config() const { return m_config; }

  // Bounded solver: optional step limit (0 = unbounded). When the bound is
  // reached, the solver stops and returns a partial result; no exception is
  // thrown.
  void set_max_steps(size_t max_steps) { m_max_steps = max_steps; }
  size_t get_max_steps() const { return m_max_steps; }
  size_t get_steps_performed() const { return m_steps_performed; }
  bool bound_reached() const { return m_bound_reached; }

  // Query interface for analysis results
  FactSet get_facts_at_entry(const llvm::Instruction *inst) const;
  FactSet get_facts_at_exit(const llvm::Instruction *inst) const;

  /// Returns facts at the given instruction in LLVM SSA style: for non-void
  /// instructions (e.g. load), returns facts at the successor instruction(s)
  /// where the defined value is valid; for void (e.g. terminator), returns
  /// facts at exit of this instruction.
  FactSet get_facts_at_in_llvm_ssa(const llvm::Instruction *inst) const;

  // Get all path edges (for debugging/analysis)
  void get_path_edges(std::vector<PathEdge<Fact>> &out_edges) const;

  // Get all summary edges (for debugging/analysis)
  void get_summary_edges(std::vector<SummaryEdge<Fact>> &out_edges) const;

  // Check if a fact reaches a specific instruction
  bool fact_reaches(const Fact &fact, const llvm::Instruction *inst) const;

  // Legacy compatibility methods for existing tools
  std::unordered_map<Node, FactSet, NodeHash> get_all_results() const;
  FactSet get_facts_at(const Node &node) const;

private:
  using PathEdgeType = PathEdge<Fact>;
  using SummaryEdgeType = SummaryEdge<Fact>;

  Problem &m_problem;
  bool m_show_progress = false;
  IFDSIDESolverConfig m_config;

  // Bounded solver state (0 = unbounded)
  size_t m_max_steps = 0;
  size_t m_steps_performed = 0;
  bool m_bound_reached = false;

  // Simple sequential data structures (no thread-safety needed)
  std::set<PathEdgeType> m_path_edges;
  std::set<SummaryEdgeType> m_summary_edges;
  std::vector<PathEdgeType> m_worklist;
  std::unordered_map<const llvm::Instruction *, FactSet> m_entry_facts;
  std::unordered_map<const llvm::Instruction *, FactSet> m_exit_facts;

  // Summary edges: keyed by (callee, entry_fact) -> set of return_fact
  using SummaryKey = std::pair<const llvm::Function *, Fact>;
  struct SummaryKeyHash {
    size_t operator()(const SummaryKey &k) const {
      return std::hash<const llvm::Function *>{}(k.first) ^
             (std::hash<Fact>{}(k.second) << 1);
    }
  };
  std::unordered_map<SummaryKey, std::set<Fact>, SummaryKeyHash> m_summaries;

  // Track entry facts used when entering each callee: (call, entry_fact) ->
  // true This allows proper retroactive summary application
  using EntryFactKey = std::pair<const llvm::CallBase *, Fact>;
  struct EntryFactKeyHash {
    size_t operator()(const EntryFactKey &k) const {
      return std::hash<const llvm::CallBase *>{}(k.first) ^
             (std::hash<Fact>{}(k.second) << 1);
    }
  };
  std::unordered_set<EntryFactKey, EntryFactKeyHash> m_entry_facts_at_call;

  // Track the call edge used to enter: (callee, entry_fact) -> original call
  // edge This allows restoring caller context when processing returns
  struct CallEdgeInfo {
    const llvm::CallBase *call_node;
    Fact call_fact;
    const llvm::Instruction *source_node;
    Fact source_fact;

    bool operator==(const CallEdgeInfo &other) const {
      return call_node == other.call_node && call_fact == other.call_fact &&
             source_node == other.source_node &&
             source_fact == other.source_fact;
    }
  };
  struct CallEdgeInfoHash {
    size_t operator()(const CallEdgeInfo &k) const {
      size_t h1 = std::hash<const llvm::CallBase *>{}(k.call_node);
      size_t h2 = std::hash<Fact>{}(k.call_fact);
      size_t h3 = std::hash<const llvm::Instruction *>{}(k.source_node);
      size_t h4 = std::hash<Fact>{}(k.source_fact);
      return ((h1 ^ (h2 << 1)) ^ (h3 << 2)) ^ (h4 << 3);
    }
  };
  using CallEdgeKey = std::pair<const llvm::Function *, Fact>;
  struct CallEdgeKeyHash {
    size_t operator()(const CallEdgeKey &k) const {
      return std::hash<const llvm::Function *>{}(k.first) ^
             (std::hash<Fact>{}(k.second) << 1);
    }
  };
  std::unordered_map<CallEdgeKey, std::vector<CallEdgeInfo>, CallEdgeKeyHash>
      m_call_edge_info;

  std::unordered_map<const llvm::Instruction *, std::set<PathEdgeType>>
      m_path_edges_at;

  // Flow function result caches (key -> FactSet) to avoid recomputation
  using NormalFlowKey = std::pair<const llvm::Instruction *, Fact>;
  using CallToReturnFlowKey = std::pair<const llvm::CallBase *, Fact>;
  std::unordered_map<NormalFlowKey, FactSet,
                     PairHash<const llvm::Instruction *, Fact>>
      m_normal_flow_cache;
  std::unordered_map<CallToReturnFlowKey, FactSet,
                     PairHash<const llvm::CallBase *, Fact>>
      m_call_to_return_flow_cache;

  // Call graph information (read-only after initialization)
  std::unordered_map<const llvm::CallBase *, std::vector<const llvm::Function *>>
      m_call_to_callee;
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::CallBase *>>
      m_callee_to_calls;
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::ReturnInst *>>
      m_function_returns;

  // CFG navigation helpers (read-only after initialization)
  std::unordered_map<const llvm::Instruction *,
                     std::vector<const llvm::Instruction *>>
      m_successors;
  std::unordered_map<const llvm::Instruction *,
                     std::vector<const llvm::Instruction *>>
      m_predecessors;
  std::unique_ptr<::dataflow::controlflow::LLVMInterCFG> m_icfg;

  // Core IFDS Tabulation Algorithm Methods
  bool propagate_path_edge(const PathEdgeType &edge);
  void process_normal_edge(const PathEdgeType &current_edge,
                           const llvm::Instruction *next);
  void process_call_edge(const PathEdgeType &current_edge,
                         const llvm::CallBase *call,
                         const llvm::Function *callee);
  void process_return_edge(const PathEdgeType &current_edge,
                           const llvm::ReturnInst *ret);
  void process_call_to_return_edge(const PathEdgeType &current_edge,
                                   const llvm::CallBase *call);

  // Helper methods: return sites = all CFG successors of the call (e.g. normal
  // + unwind for invoke)
  std::vector<const llvm::Instruction *>
  get_return_sites(const llvm::CallBase *call) const;
  std::vector<const llvm::Instruction *>
  get_successors(const llvm::Instruction *inst) const;

  // Initialization methods
  void initialize_call_graph(const llvm::Module &module);
  void build_cfg_successors(const llvm::Module &module);
  void initialize_worklist(const llvm::Module &module);
  void run_tabulation();

  const llvm::Function *get_main_function(const llvm::Module &module);
};

} // namespace ifds

#include "Dataflow/IFDS/Solvers/IFDSSolver.tpp"
