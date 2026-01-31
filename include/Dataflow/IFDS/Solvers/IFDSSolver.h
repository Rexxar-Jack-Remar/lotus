/*
 * IFDS Solver (Sequential)
 *
 * This header provides the sequential IFDS tabulation solver.
 */

#pragma once

#include "Dataflow/IFDS/IFDSFramework.h"

namespace ifds {

// ============================================================================
// IFDS Solver (Sequential)
// ============================================================================

template<typename Problem>
class IFDSSolver {
public:
    using Fact = typename Problem::FactType;
    using FactSet = typename Problem::FactSet;
    using Node = typename ExplodedSupergraph<Fact>::Node;
    using NodeHash = typename ExplodedSupergraph<Fact>::NodeHash;

    IFDSSolver(Problem& problem);

    void solve(const llvm::Module& module);

    // Enable/disable progress bar display during analysis
    void set_show_progress(bool show) { m_show_progress = show; }

    // Query interface for analysis results
    FactSet get_facts_at_entry(const llvm::Instruction* inst) const;
    FactSet get_facts_at_exit(const llvm::Instruction* inst) const;

    // Get all path edges (for debugging/analysis)
    void get_path_edges(std::vector<PathEdge<Fact>>& out_edges) const;

    // Get all summary edges (for debugging/analysis)
    void get_summary_edges(std::vector<SummaryEdge<Fact>>& out_edges) const;

    // Check if a fact reaches a specific instruction
    bool fact_reaches(const Fact& fact, const llvm::Instruction* inst) const;

    // Legacy compatibility methods for existing tools
    std::unordered_map<Node, FactSet, NodeHash> get_all_results() const;
    FactSet get_facts_at(const Node& node) const;

private:
    using PathEdgeType = PathEdge<Fact>;
    using SummaryEdgeType = SummaryEdge<Fact>;

    Problem& m_problem;
    bool m_show_progress = false;

    // Simple sequential data structures (no thread-safety needed)
    std::set<PathEdgeType> m_path_edges;
    std::set<SummaryEdgeType> m_summary_edges;
    std::vector<PathEdgeType> m_worklist;
    std::unordered_map<const llvm::Instruction*, FactSet> m_entry_facts;
    std::unordered_map<const llvm::Instruction*, FactSet> m_exit_facts;

    // Indexed data structures for fast lookup
    std::unordered_map<const llvm::CallInst*, std::set<SummaryEdgeType>> m_summary_index;
    std::unordered_map<const llvm::Instruction*, std::set<PathEdgeType>> m_path_edges_at;

    // Call graph information (read-only after initialization)
    std::unordered_map<const llvm::CallInst*, const llvm::Function*> m_call_to_callee;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> m_callee_to_calls;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::ReturnInst*>> m_function_returns;

    // CFG navigation helpers (read-only after initialization)
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> m_successors;
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> m_predecessors;

    // Core IFDS Tabulation Algorithm Methods
    bool propagate_path_edge(const PathEdgeType& edge);
    void process_normal_edge(const PathEdgeType& current_edge, const llvm::Instruction* next);
    void process_call_edge(const PathEdgeType& current_edge, const llvm::CallInst* call, const llvm::Function* callee);
    void process_return_edge(const PathEdgeType& current_edge, const llvm::ReturnInst* ret);
    void process_call_to_return_edge(const PathEdgeType& current_edge, const llvm::CallInst* call);

    // Helper methods
    const llvm::Instruction* get_return_site(const llvm::CallInst* call) const;
    std::vector<const llvm::Instruction*> get_successors(const llvm::Instruction* inst) const;

    // Initialization methods
    void initialize_call_graph(const llvm::Module& module);
    void build_cfg_successors(const llvm::Module& module);
    void initialize_worklist(const llvm::Module& module);
    void run_tabulation();

    const llvm::Function* get_main_function(const llvm::Module& module);
};

} // namespace ifds

#include "Dataflow/IFDS/Solvers/IFDSSolver.tpp"
