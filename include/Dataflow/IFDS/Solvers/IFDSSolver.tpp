/*
 * Sequential IFDS Solver Implementation
 *
 * This implements a straightforward sequential version of the IFDS tabulation algorithm:
 * - Simple worklist-based processing
 * - No thread synchronization overhead
 * - Easier to debug and maintain
 * - Suitable for small to medium programs or debugging
 */

#include "Utils/Platform/ProgressBar.h"

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
    std::unique_ptr<lotus::AliasAnalysisWrapper> owned_alias_analysis;
    if (m_config.auto_inject_alias_analysis() &&
        !m_problem.has_alias_analysis_configured()) {
        owned_alias_analysis = std::make_unique<lotus::AliasAnalysisWrapper>(
            const_cast<llvm::Module&>(module),
            m_config.alias_analysis_config());
        m_problem.set_alias_analysis(owned_alias_analysis.get());
    }

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

    // Preserve context precision: process each newly discovered path edge,
    // even if another start fact already reached the same (target_node, target_fact).
    m_entry_facts[edge.target_node].insert(edge.target_fact);
    m_worklist.push_back(edge);

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
                                            const llvm::CallBase* call,
                                            const llvm::Function* callee) {
    if (!callee || callee->isDeclaration() || callee->empty()) {
        return;
    }

    // Get callee entry point
    const llvm::Instruction* callee_entry = &callee->getEntryBlock().front();

    // Apply call flow function to get facts at callee entry
    FactSet call_facts = m_problem.call_flow(call, callee, current_edge.target_fact);
    if (m_problem.auto_add_zero() && m_problem.is_zero_fact(current_edge.target_fact)) {
        call_facts.insert(m_problem.zero_fact());
    }

    for (const auto& entry_fact : call_facts) {
        // Track the entry fact used for this call
        m_entry_facts_at_call.insert({call, entry_fact});

        // Track call edge info for restoring caller context on return.
        // Use the caller's fact at the call site (current_edge.target_fact),
        // not the callee's entry fact, as the call_fact field.
        CallEdgeInfo edge_info{call, current_edge.target_fact,
                               current_edge.start_node, current_edge.start_fact};
        auto& info_vec = m_call_edge_info[{callee, entry_fact}];
        // Deduplicate: only add if this exact call edge is not already recorded.
        // Without deduplication, re-visiting the same call instruction (e.g.
        // due to multiple path edges reaching it) would push duplicate entries
        // and cause the same return-site propagation to fire multiple times.
        bool already_recorded = false;
        for (const auto& existing : info_vec) {
            if (existing == edge_info) {
                already_recorded = true;
                break;
            }
        }
        if (!already_recorded) {
            info_vec.push_back(edge_info);
        }

        // Propagate into callee
        propagate_path_edge(PathEdgeType(callee_entry, entry_fact, callee_entry, entry_fact));

        // Apply existing summaries for this (callee, entry_fact) combination
        // This handles the case where the callee was already analyzed
        auto summary_it = m_summaries.find({callee, entry_fact});
        if (summary_it != m_summaries.end()) {
            for (const Fact& return_fact : summary_it->second) {
                FactSet return_facts = m_problem.return_flow(call, callee, return_fact, entry_fact);
                if (m_problem.auto_add_zero() && m_problem.is_zero_fact(return_fact)) {
                    return_facts.insert(m_problem.zero_fact());
                }
                for (const llvm::Instruction* return_site : get_return_sites(call)) {
                    for (const auto& rf : return_facts) {
                        propagate_path_edge(PathEdgeType(current_edge.start_node, current_edge.start_fact,
                                                         return_site, rf));
                    }
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_return_edge(const PathEdgeType& current_edge,
                                              const llvm::ReturnInst* ret) {
    const llvm::Function* func = ret->getFunction();
    if (!func || func->empty()) {
        return;
    }

    const Fact& exit_fact = current_edge.target_fact;
    const Fact& start_fact = current_edge.start_fact;  // This is the entry fact
    bool had_incoming = false;

    // Create the summary for this (callee, entry_fact) combination
    SummaryKey summary_key{func, start_fact};
    auto& return_facts_set = m_summaries[summary_key];
    return_facts_set.insert(exit_fact);

    // Look up the call edge info to restore caller context (may have multiple call sites)
    auto call_edge_it = m_call_edge_info.find({func, start_fact});
    if (call_edge_it != m_call_edge_info.end()) {
        had_incoming = true;
        for (const CallEdgeInfo& edge_info : call_edge_it->second) {
            // Compute return facts using the return flow function.
            // The fourth argument is the caller's fact at the call site
            // (edge_info.call_fact), NOT the callee's entry fact (start_fact).
            // Passing start_fact here was wrong: it gave the flow function the
            // callee's entry fact instead of the caller's context fact, which
            // breaks analyses that use call_fact to decide what to propagate
            // back (e.g. taint analysis killing non-tainted return paths).
            FactSet return_facts = m_problem.return_flow(edge_info.call_node, func, exit_fact, edge_info.call_fact);
            if (m_problem.auto_add_zero() && m_problem.is_zero_fact(exit_fact)) {
                return_facts.insert(m_problem.zero_fact());
            }

            // SummaryEdge records (call_site, caller_fact_at_call, callee_exit_fact).
            // Use edge_info.call_fact (the caller's fact at the call site) rather
            // than start_fact (the callee's entry fact) so that the summary edge
            // correctly reflects the caller's context.
            SummaryEdgeType new_summary(edge_info.call_node, edge_info.call_fact, exit_fact);
            m_summary_edges.insert(new_summary);

            for (const llvm::Instruction* return_site : get_return_sites(edge_info.call_node)) {
                for (const Fact& return_fact : return_facts) {
                    propagate_path_edge(PathEdgeType(edge_info.source_node, edge_info.source_fact,
                                                    return_site, return_fact));
                }
            }
        }
    }

    // Unbalanced returns: the callee was seeded directly (no incoming call edge
    // recorded for this entry fact).  Propagate return facts to all known
    // callers' return sites, using the caller's own start context (zero fact as
    // the path-edge start, which is the conventional IFDS treatment for
    // unbalanced returns).  We do NOT create a self-loop at the return site;
    // instead we propagate from the call site's start node so that the result
    // is visible in the caller's context.
    auto callee_calls_it = m_callee_to_calls.find(func);
    if (m_config.follow_returns_past_seeds() && !had_incoming &&
        callee_calls_it != m_callee_to_calls.end()) {
        Fact zero = m_problem.zero_fact();
        for (const llvm::CallBase* call : callee_calls_it->second) {
            FactSet return_facts = m_problem.return_flow(call, func, exit_fact, zero);
            if (m_problem.auto_add_zero()) {
                return_facts.insert(zero);
            }
            for (const llvm::Instruction* return_site : get_return_sites(call)) {
                for (const Fact& rf : return_facts) {
                    // Propagate with zero as the start fact so the result is
                    // anchored at the return site in the caller's context,
                    // not as a spurious self-loop seed.
                    propagate_path_edge(
                        PathEdgeType(return_site, zero, return_site, rf));
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_call_to_return_edge(const PathEdgeType& current_edge,
                                                      const llvm::CallBase* call) {
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
IFDSSolver<Problem>::get_return_sites(const llvm::CallBase* call) const {
    if (m_icfg == nullptr || call == nullptr) {
        return {};
    }
    std::vector<const llvm::Instruction*> result;
    for (auto* site : m_icfg->getReturnSitesOfCallAt(
             const_cast<llvm::CallBase*>(call))) {
        if (site != nullptr) {
            result.push_back(site);
        }
    }
    return result;
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
    m_icfg = std::make_unique<::dataflow::controlflow::LLVMInterCFG>(
        const_cast<llvm::Module*>(&module));

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;

        std::vector<const llvm::ReturnInst*> returns;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                    returns.push_back(ret);
                } else if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
                    const auto callee_vec =
                        m_icfg->getCalleesOfCallAt(const_cast<llvm::CallBase*>(call));
                    std::vector<const llvm::Function*> callees;
                    std::unordered_set<const llvm::Function*> seen;
                    for (const llvm::Function* callee : callee_vec) {
                        if (!callee || !seen.insert(callee).second) {
                            continue;
                        }
                        callees.push_back(callee);
                        m_callee_to_calls[callee].push_back(call);
                    }
                    if (!callees.empty()) {
                        m_call_to_callee[call] = std::move(callees);
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

                for (auto* succ : m_icfg->getSuccsOf(
                         const_cast<llvm::Instruction*>(&inst),
                         ::dataflow::controlflow::FlowDirection::Forward)) {
                    if (succ != nullptr) {
                        succs.push_back(succ);
                    }
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
    m_summaries.clear();
    m_entry_facts_at_call.clear();
    m_call_edge_info.clear();
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
    // Use unique_ptr so the ProgressBar is always destroyed even if an
    // exception propagates out of the loop (exception-safe resource management).
    std::unique_ptr<ProgressBar> progress;
    size_t processed_edges = 0;
    size_t last_update = 0;
    const size_t update_interval = 100;

    if (m_show_progress) {
        progress = std::make_unique<ProgressBar>(
            "Sequential IFDS Analysis", ProgressBar::PBS_CharacterStyle, 0.01);
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
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(curr)) {
            // Call-to-return flows are always processed once per call edge.
            process_call_to_return_edge(current_edge, call);
            auto it = m_call_to_callee.find(call);
            if (it != m_call_to_callee.end()) {
                for (const llvm::Function* callee : it->second) {
                    process_call_edge(current_edge, call, callee);
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
        // progress is destroyed automatically by unique_ptr
    }
}

template<typename Problem>
const llvm::Function* IFDSSolver<Problem>::get_main_function(const llvm::Module& module) {
    return module.getFunction("main");
}

} // namespace ifds
