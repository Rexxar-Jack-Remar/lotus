/*
 * Sequential IFDS Solver Implementation
 *
 * This implements a straightforward sequential version of the IFDS tabulation algorithm:
 * - Simple worklist-based processing
 * - No thread synchronization overhead
 * - Easier to debug and maintain
 * - Suitable for small to medium programs or debugging
 */

#include "Utils/General/ProgressBar.h"

#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// IFDSSolver Implementation
// ============================================================================

template<typename Problem>
IFDSSolver<Problem>::IFDSSolver(Problem& problem)
    : m_problem(problem) {
}

template<typename Problem>
void IFDSSolver<Problem>::solve(const llvm::Module& module) {
    m_steps_performed = 0;
    m_bound_reached = false;

    // Initialize data structures
    initialize_call_graph(module);
    build_cfg_successors(module);
    initialize_worklist(module);

    // Run sequential tabulation algorithm
    run_tabulation();
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet
IFDSSolver<Problem>::get_facts_at_entry(const llvm::Instruction* inst) const {
    auto it = m_entry_facts.find(inst);
    return it != m_entry_facts.end() ? it->second : FactSet{};
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet
IFDSSolver<Problem>::get_facts_at_exit(const llvm::Instruction* inst) const {
    auto it = m_exit_facts.find(inst);
    return it != m_exit_facts.end() ? it->second : FactSet{};
}

template<typename Problem>
void IFDSSolver<Problem>::get_path_edges(std::vector<PathEdge<Fact>>& out_edges) const {
    out_edges.clear();
    out_edges.reserve(m_path_edges.size());
    for (const auto& edge : m_path_edges) {
        out_edges.push_back(edge);
    }
}

template<typename Problem>
void IFDSSolver<Problem>::get_summary_edges(std::vector<SummaryEdge<Fact>>& out_edges) const {
    out_edges.clear();
    out_edges.reserve(m_summary_edges.size());
    for (const auto& edge : m_summary_edges) {
        out_edges.push_back(edge);
    }
}

template<typename Problem>
bool IFDSSolver<Problem>::fact_reaches(const Fact& fact, const llvm::Instruction* inst) const {
    auto it = m_exit_facts.find(inst);
    return it != m_exit_facts.end() && it->second.find(fact) != it->second.end();
}

template<typename Problem>
std::unordered_map<typename IFDSSolver<Problem>::Node,
                  typename IFDSSolver<Problem>::FactSet,
                  typename IFDSSolver<Problem>::NodeHash>
IFDSSolver<Problem>::get_all_results() const {
    std::unordered_map<Node, FactSet, NodeHash> results;
    typename Problem::FactType zero = m_problem.zero_fact();

    for (const auto& pair : m_exit_facts) {
        const llvm::Instruction* inst = pair.first;
        const FactSet& facts = pair.second;
        if (!facts.empty()) {
            results[Node(inst, zero)] = facts;
        }
    }

    return results;
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet
IFDSSolver<Problem>::get_facts_at(const Node& node) const {
    return get_facts_at_exit(node.instruction);
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet
IFDSSolver<Problem>::get_facts_at_in_llvm_ssa(const llvm::Instruction* inst) const {
    if (inst->getType()->isVoidTy()) {
        return get_facts_at_exit(inst);
    }
    const llvm::Instruction* next = inst->getNextNode();
    if (next) {
        return get_facts_at_entry(next);
    }
    if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(inst)) {
        llvm::BasicBlock* normal = invoke->getNormalDest();
        if (normal && !normal->empty()) {
            return get_facts_at_entry(&normal->front());
        }
    }
    return get_facts_at_exit(inst);
}

// ============================================================================
// Core IFDS Tabulation Algorithm Methods
// ============================================================================

template<typename Problem>
bool IFDSSolver<Problem>::propagate_path_edge(const PathEdgeType& edge) {
    // Try to insert the edge - if already exists, return false
    if (!m_path_edges.insert(edge).second) {
        return false;
    }

    // Add to worklist for processing
    m_worklist.push_back(edge);

    // Track facts at instruction entry.
    m_entry_facts[edge.target_node].insert(edge.target_fact);

    // Maintain path-edge index P[c] for retroactive summary application
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(edge.target_node)) {
        m_path_edges_at[edge.target_node].insert(edge);

        // Apply existing summaries (stored at callee) to this new path edge
        auto callee_it = m_call_to_callee.find(call);
        const llvm::Function* callee = (callee_it != m_call_to_callee.end())
            ? callee_it->second : nullptr;
        if (callee && !callee->isDeclaration() && !callee->empty()) {
            auto summary_it = m_summary_by_callee.find({callee, edge.target_fact});
            if (summary_it != m_summary_by_callee.end()) {
                for (const llvm::Instruction* return_site : get_return_sites(call)) {
                    for (const Fact& return_fact : summary_it->second) {
                        FactSet return_facts = m_problem.return_flow(call, callee,
                                                                     return_fact, edge.target_fact);
                        for (const auto& rf : return_facts) {
                            propagate_path_edge(PathEdgeType(edge.start_node, edge.start_fact,
                                                             return_site, rf));
                        }
                    }
                }
            }
        }
    }

    return true;
}

template<typename Problem>
void IFDSSolver<Problem>::process_normal_edge(const PathEdgeType& current_edge,
                                              const llvm::Instruction* next) {
    NormalFlowKey nkey{current_edge.target_node, current_edge.target_fact};
    auto cit = m_normal_flow_cache.find(nkey);
    FactSet new_facts;
    if (cit != m_normal_flow_cache.end()) {
        new_facts = cit->second;
    } else {
        new_facts = m_problem.normal_flow(current_edge.target_node, current_edge.target_fact);
        if (m_problem.auto_add_zero() && m_problem.is_zero_fact(current_edge.target_fact)) {
            new_facts.insert(m_problem.zero_fact());
        }
        m_normal_flow_cache[nkey] = new_facts;
    }

    // Record exit facts for the current instruction.
    if (!new_facts.empty()) {
        auto& exit_facts = m_exit_facts[current_edge.target_node];
        exit_facts.insert(new_facts.begin(), new_facts.end());
    }

    for (const auto& new_fact : new_facts) {
        propagate_path_edge(PathEdgeType(current_edge.start_node, current_edge.start_fact,
                                         next, new_fact));
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_call_edge(const PathEdgeType& current_edge,
                                           const llvm::CallInst* call,
                                           const llvm::Function* callee) {
    // ALWAYS generate call-to-return edges (textbook IFDS requirement)
    process_call_to_return_edge(current_edge, call);

    if (!callee || callee->isDeclaration() || callee->empty()) {
        return;
    }

    // Get callee entry point
    const llvm::Instruction* callee_entry = &callee->getEntryBlock().front();

    // Apply call flow function
    FactSet call_facts = m_problem.call_flow(call, callee, current_edge.target_fact);
    if (m_problem.auto_add_zero() && m_problem.is_zero_fact(current_edge.target_fact)) {
        call_facts.insert(m_problem.zero_fact());
    }

    for (const auto& call_fact : call_facts) {
        propagate_path_edge(PathEdgeType(callee_entry, call_fact, callee_entry, call_fact));
    }

    // Apply existing summaries (stored at callee) retroactively
    auto summary_it = m_summary_by_callee.find({callee, current_edge.target_fact});
    if (summary_it != m_summary_by_callee.end()) {
        for (const llvm::Instruction* return_site : get_return_sites(call)) {
            for (const Fact& return_fact : summary_it->second) {
                FactSet return_facts = m_problem.return_flow(call, callee,
                                                             return_fact, current_edge.target_fact);
                if (m_problem.auto_add_zero() && m_problem.is_zero_fact(return_fact)) {
                    return_facts.insert(m_problem.zero_fact());
                }
                for (const auto& rf : return_facts) {
                    propagate_path_edge(PathEdgeType(current_edge.start_node, current_edge.start_fact,
                                                     return_site, rf));
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_return_edge(const PathEdgeType& current_edge,
                                              const llvm::ReturnInst* ret) {
    const llvm::Function* func = ret->getFunction();
    const llvm::Instruction* callee_entry = func && !func->empty()
        ? &func->getEntryBlock().front() : nullptr;
    const Fact& exit_fact = current_edge.target_fact;
    const Fact& start_fact = current_edge.start_fact;
    bool had_incoming = false;

    // Find all call sites for this function
    auto it = m_callee_to_calls.find(func);
    if (it != m_callee_to_calls.end()) {
        for (const llvm::CallInst* call : it->second) {
            auto path_it = m_path_edges_at.find(call);
            if (path_it == m_path_edges_at.end()) continue;
            for (const auto& path_edge : path_it->second) {
                had_incoming = true;
                const Fact& call_fact = path_edge.target_fact;

                SummaryEdgeType new_summary(call, call_fact, exit_fact);
                if (!m_summary_edges.insert(new_summary).second) continue;

                m_summary_by_callee[{func, call_fact}].insert(exit_fact);
                FactSet return_facts = m_problem.return_flow(call, func, exit_fact, call_fact);
                if (m_problem.auto_add_zero() && m_problem.is_zero_fact(exit_fact)) {
                    return_facts.insert(m_problem.zero_fact());
                }
                for (const llvm::Instruction* return_site : get_return_sites(call)) {
                    for (const Fact& return_fact : return_facts) {
                        propagate_path_edge(PathEdgeType(path_edge.start_node, path_edge.start_fact,
                                                         return_site, return_fact));
                    }
                }
            }
        }
    }

    // Unbalanced returns: propagate to all return sites of all callers with zero fact when
    // this return had no incoming call edge (e.g. entry-point function returning).
    if (m_config.follow_returns_past_seeds() && !had_incoming && callee_entry && it != m_callee_to_calls.end()) {
        Fact zero = m_problem.zero_fact();
        for (const llvm::CallInst* call : it->second) {
            FactSet return_facts = m_problem.return_flow(call, func, exit_fact, zero);
            if (m_problem.auto_add_zero()) {
                return_facts.insert(zero);
            }
            for (const llvm::Instruction* return_site : get_return_sites(call)) {
                for (const Fact& rf : return_facts) {
                    propagate_path_edge(PathEdgeType(return_site, zero, return_site, rf));
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_call_to_return_edge(const PathEdgeType& current_edge,
                                                      const llvm::CallInst* call) {
    CallToReturnFlowKey ckey{call, current_edge.target_fact};
    auto cit = m_call_to_return_flow_cache.find(ckey);
    FactSet ctr_facts;
    if (cit != m_call_to_return_flow_cache.end()) {
        ctr_facts = cit->second;
    } else {
        ctr_facts = m_problem.call_to_return_flow(call, current_edge.target_fact);
        if (m_problem.auto_add_zero() && m_problem.is_zero_fact(current_edge.target_fact)) {
            ctr_facts.insert(m_problem.zero_fact());
        }
        m_call_to_return_flow_cache[ckey] = ctr_facts;
    }

    // Record exit facts for the call instruction (call-to-return flow).
    if (!ctr_facts.empty()) {
        auto& exit_facts = m_exit_facts[call];
        exit_facts.insert(ctr_facts.begin(), ctr_facts.end());
    }

    for (const llvm::Instruction* return_site : get_return_sites(call)) {
        for (const auto& ctr_fact : ctr_facts) {
            propagate_path_edge(PathEdgeType(current_edge.start_node, current_edge.start_fact,
                                             return_site, ctr_fact));
        }
    }
}

// ============================================================================
// Helper Methods
// ============================================================================

template<typename Problem>
std::vector<const llvm::Instruction*>
IFDSSolver<Problem>::get_return_sites(const llvm::CallInst* call) const {
    auto it = m_successors.find(call);
    if (it != m_successors.end()) {
        return it->second;
    }
    return {};
}

template<typename Problem>
std::vector<const llvm::Instruction*>
IFDSSolver<Problem>::get_successors(const llvm::Instruction* inst) const {
    auto it = m_successors.find(inst);
    if (it != m_successors.end()) {
        return it->second;
    }
    return {};
}

// ============================================================================
// Initialization Methods
// ============================================================================

template<typename Problem>
void IFDSSolver<Problem>::initialize_call_graph(const llvm::Module& module) {
    m_call_to_callee.clear();
    m_callee_to_calls.clear();
    m_function_returns.clear();

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;

        std::vector<const llvm::ReturnInst*> returns;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                    returns.push_back(ret);
                } else if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (const llvm::Function* callee = call->getCalledFunction()) {
                        m_call_to_callee[call] = callee;
                        m_callee_to_calls[callee].push_back(call);
                    }
                }
            }
        }
        m_function_returns[&func] = returns;
    }
}

template<typename Problem>
void IFDSSolver<Problem>::build_cfg_successors(const llvm::Module& module) {
    m_successors.clear();
    m_predecessors.clear();

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;

        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                std::vector<const llvm::Instruction*> succs;

                if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                    for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
                        llvm::BasicBlock* succ = br->getSuccessor(i);
                        if (succ && !succ->empty()) {
                            succs.push_back(&succ->front());
                        }
                    }
                } else if (auto* sw = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
                    for (unsigned i = 0; i < sw->getNumSuccessors(); ++i) {
                        llvm::BasicBlock* succ = sw->getSuccessor(i);
                        if (succ && !succ->empty()) {
                            succs.push_back(&succ->front());
                        }
                    }
                } else if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&inst)) {
                    llvm::BasicBlock* normalDest = invoke->getNormalDest();
                    if (normalDest && !normalDest->empty()) {
                        succs.push_back(&normalDest->front());
                    }
                    llvm::BasicBlock* unwindDest = invoke->getUnwindDest();
                    if (unwindDest && !unwindDest->empty()) {
                        succs.push_back(&unwindDest->front());
                    }
                } else if (llvm::isa<llvm::ReturnInst>(&inst) ||
                          llvm::isa<llvm::UnreachableInst>(&inst)) {
                    // No intraprocedural successors
                } else if (const llvm::Instruction* next = inst.getNextNode()) {
                    succs.push_back(next);
                }

                m_successors[&inst] = succs;

                for (const llvm::Instruction* succ : succs) {
                    m_predecessors[succ].push_back(&inst);
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::initialize_worklist(const llvm::Module& module) {
    m_path_edges.clear();
    m_summary_edges.clear();
    m_worklist.clear();
    m_entry_facts.clear();
    m_exit_facts.clear();
    m_summary_by_callee.clear();
    m_path_edges_at.clear();
    m_normal_flow_cache.clear();
    m_call_to_return_flow_cache.clear();

    auto seeds = m_problem.initial_seeds(module);
    if (seeds.empty()) {
        const llvm::Function* main_func = get_main_function(module);
        if (!main_func) {
            for (const llvm::Function& func : module) {
                if (!func.isDeclaration() && !func.empty()) {
                    main_func = &func;
                    break;
                }
            }
        }
        if (main_func && !main_func->empty()) {
            const llvm::Instruction* entry = &main_func->getEntryBlock().front();
            seeds.add_seed(entry, m_problem.initial_facts(main_func));
        }
    }

    for (const auto& pair : seeds.get_seeds()) {
        const llvm::Instruction* entry = pair.first;
        FactSet facts = pair.second;
        if (m_problem.auto_add_zero()) {
            bool has_zero = false;
            for (const auto& fact : facts) {
                if (m_problem.is_zero_fact(fact)) {
                    has_zero = true;
                    break;
                }
            }
            if (!has_zero) {
                facts.insert(m_problem.zero_fact());
            }
        }
        for (const auto& fact : facts) {
            propagate_path_edge(PathEdgeType(entry, fact, entry, fact));
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::run_tabulation() {
    ProgressBar* progress = nullptr;
    size_t processed_edges = 0;
    size_t last_update = 0;
    const size_t update_interval = 100;

    if (m_show_progress) {
        progress = new ProgressBar("Sequential IFDS Analysis", ProgressBar::PBS_CharacterStyle, 0.01);
        llvm::outs() << "\n";
    }

    while (!m_worklist.empty()) {
        if (m_max_steps != 0 && m_steps_performed >= m_max_steps) {
            m_bound_reached = true;
            break;
        }

        PathEdgeType current_edge = m_worklist.back();
        m_worklist.pop_back();

        const llvm::Instruction* curr = current_edge.target_node;

        // Process different instruction types
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(curr)) {
            if (!llvm::isa<llvm::InvokeInst>(curr)) {
                auto it = m_call_to_callee.find(call);
                if (it != m_call_to_callee.end()) {
                    process_call_edge(current_edge, call, it->second);
                } else {
                    process_call_to_return_edge(current_edge, call);
                }
            } else {
                auto* invoke = llvm::cast<llvm::InvokeInst>(curr);
                if (const llvm::Function* callee = invoke->getCalledFunction()) {
                    process_call_edge(current_edge, call, callee);
                } else {
                    process_call_to_return_edge(current_edge, call);
                }
            }
        } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(curr)) {
            process_return_edge(current_edge, ret);
        } else {
            auto succs = get_successors(curr);
            for (const llvm::Instruction* succ : succs) {
                process_normal_edge(current_edge, succ);
            }
        }

        processed_edges++;
        m_steps_performed = processed_edges;

        if (m_show_progress && processed_edges - last_update >= update_interval) {
            last_update = processed_edges;
            size_t total_path_edges = m_path_edges.size();
            size_t worklist_size = m_worklist.size();

            llvm::outs() << "\r\033[KProcessed: " << processed_edges
                        << " | Path edges: " << total_path_edges
                        << " | Worklist: " << worklist_size;
            llvm::outs().flush();
        }
    }

    if (m_show_progress) {
        llvm::outs() << "\r\033[K";
        progress->showProgress(1.0);
        llvm::outs() << "\nCompleted! Processed " << processed_edges
                    << " edges, discovered " << m_path_edges.size() << " path edges";
        if (m_bound_reached) {
            llvm::outs() << " (step bound " << m_max_steps << " reached)";
        }
        llvm::outs() << "\n";
        delete progress;
    }
}

template<typename Problem>
const llvm::Function* IFDSSolver<Problem>::get_main_function(const llvm::Module& module) {
    return module.getFunction("main");
}

} // namespace ifds
