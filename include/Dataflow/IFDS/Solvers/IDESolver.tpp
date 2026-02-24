/*
 * IDE Solver Implementation
 *
 * This implements the IDE (Interprocedural Distributive Environment) algorithm,
 * an extension of IFDS that propagates values along with dataflow facts.
 *
 * Key features:
 * - Summary edge reuse: Callees are analyzed once per calling context
 * - Edge function composition memoization: Avoids redundant function compositions
 */

#include "Dataflow/ControlFlow/InterCFG.h"
#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>

namespace ifds {

// ============================================================================
// IDESolver Implementation
// ============================================================================

template<typename Problem>
IDESolver<Problem>::IDESolver(Problem& problem) : m_problem(problem) {}

// ============================================================================
// Helper Methods
// ============================================================================

template<typename Problem>
typename IDESolver<Problem>::EdgeFunctionPtr
IDESolver<Problem>::make_edge_function(const EdgeFunction& ef) {
    return std::make_shared<EdgeFunction>(ef);
}

template<typename Problem>
typename IDESolver<Problem>::EdgeFunctionPtr
IDESolver<Problem>::compose_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2) {
    // Check cache first
    ComposePair key{f1, f2};
    auto it = m_compose_cache.find(key);
    if (it != m_compose_cache.end()) {
        return it->second;
    }

    // Compose and cache
    EdgeFunction composed = m_problem.compose(*f1, *f2);
    EdgeFunctionPtr result = make_edge_function(composed);
    m_compose_cache[key] = result;
    return result;
}

template<typename Problem>
typename IDESolver<Problem>::EdgeFunctionPtr
IDESolver<Problem>::join_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2) {
    ComposePair key{f1, f2};
    auto it = m_join_cache.find(key);
    if (it != m_join_cache.end()) {
        return it->second;
    }

    EdgeFunction joined = m_problem.join_edge_functions(*f1, *f2);
    EdgeFunctionPtr result = make_edge_function(joined);
    m_join_cache[key] = result;
    return result;
}

template<typename Problem>
void IDESolver<Problem>::solve(const llvm::Module& module) {
    using Fact = typename Problem::FactType;
    using Value = typename Problem::ValueType;

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

    // Clear previous results and caches
    m_values.clear();
    m_jump_functions.clear();
    m_incoming.clear();
    m_end_summaries.clear();
    m_compose_cache.clear();
    m_join_cache.clear();
    m_normal_edge_cache.clear();
    m_call_to_return_edge_cache.clear();
    m_worklist.clear();

    auto icfg = std::make_unique<::dataflow::controlflow::LLVMInterCFG>(
        const_cast<llvm::Module*>(&module));

    // Build call graph (supports indirect calls through ICFG resolution)
    std::unordered_map<const llvm::CallBase*, std::vector<const llvm::Function*>>
        call_to_callees;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallBase*>> callee_to_calls;

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
                    std::vector<const llvm::Function*> callees;
                    std::unordered_set<const llvm::Function*> seen;
                    for (const llvm::Function* callee :
                         icfg->getCalleesOfCallAt(const_cast<llvm::CallBase*>(call))) {
                        if (!callee || !seen.insert(callee).second) {
                            continue;
                        }
                        callees.push_back(callee);
                        callee_to_calls[callee].push_back(call);
                    }
                    if (!callees.empty()) {
                        call_to_callees[call] = std::move(callees);
                    }
                }
            }
        }
    }

    // Build CFG successors from ICFG (covers invoke/unwind and inter-IR nuances)
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> successors;
    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                std::vector<const llvm::Instruction*> succs;
                for (auto* succ : icfg->getSuccsOf(
                         const_cast<llvm::Instruction*>(&inst),
                         ::dataflow::controlflow::FlowDirection::Forward)) {
                    if (succ != nullptr) {
                        succs.push_back(succ);
                    }
                }
                successors[&inst] = std::move(succs);
            }
        }
    }

    auto get_return_sites = [&](const llvm::CallBase* call)
        -> std::vector<const llvm::Instruction*> {
        std::vector<const llvm::Instruction*> result;
        for (auto* site : icfg->getReturnSitesOfCallAt(const_cast<llvm::CallBase*>(call))) {
            if (site != nullptr) {
                result.push_back(site);
            }
        }
        return result;
    };

    auto preserve_zero = [&](FactSet& facts, const Fact& source_fact) {
        if (m_problem.auto_add_zero() && m_problem.is_zero_fact(source_fact)) {
            facts.insert(m_problem.zero_fact());
        }
    };

    EdgeFunctionPtr identity_func = make_edge_function(m_problem.identity());

    auto add_jump_function = [&](const PathEdgeType& edge, EdgeFunctionPtr phi) {
        auto it = m_jump_functions.find(edge);
        if (it == m_jump_functions.end()) {
            m_jump_functions.emplace(edge, phi);
            m_worklist.emplace_back(edge, phi);
            return;
        }

        EdgeFunctionPtr joined = join_cached(it->second, phi);
        // Use pointer identity to detect change: join_cached returns the
        // existing pointer unchanged when the join is idempotent (i.e. the
        // new phi is already subsumed).  If the pointer changed, the jump
        // function was updated and we must re-propagate.
        // The old semantic-equivalence probe (checking only top/bottom/join)
        // was unsound for multi-valued domains (e.g. integer constants, type
        // states) where two distinct functions can agree on those three probe
        // points yet differ on other inputs.
        if (it->second != joined) {
            it->second = joined;
            m_worklist.emplace_back(edge, joined);
        }
    };

    // add_incoming: record a call edge for a callee start key.
    // We do NOT store caller_phi in the incoming record because the jump
    // function for the caller path edge may be updated (joined) after the
    // incoming edge is first recorded.  Instead we store only the path-edge
    // identity (start_node, start_fact) and look up the current jump function
    // from m_jump_functions at summary-application time.
    auto add_incoming = [&](const StartKey& key, const IncomingEdge& incoming) {
        auto& list = m_incoming[key];
        // Deduplicate by structural identity (ignoring caller_phi which is
        // looked up dynamically).
        for (const auto& existing : list) {
            if (existing.call == incoming.call &&
                existing.call_fact == incoming.call_fact &&
                existing.start_node == incoming.start_node &&
                existing.start_fact == incoming.start_fact) {
                return;
            }
        }
        list.push_back(incoming);
    };

    auto add_summary = [&](const StartKey& key, const Fact& exit_fact, EdgeFunctionPtr phi) {
        auto& vec = m_end_summaries[key][exit_fact];
        if (std::find(vec.begin(), vec.end(), phi) == vec.end()) {
            vec.push_back(phi);
            return true;
        }
        return false;
    };

    auto apply_summary_to_incoming = [&](const IncomingEdge& incoming,
                                         const llvm::Function* callee,
                                         const Fact& callee_fact,
                                         const Fact& exit_fact,
                                         EdgeFunctionPtr summary_phi) {
        FactSet return_facts = m_problem.return_flow(incoming.call, callee, exit_fact, incoming.call_fact);
        preserve_zero(return_facts, exit_fact);

        auto call_ef = m_problem.call_edge_function(incoming.call, incoming.call_fact, callee_fact);
        EdgeFunctionPtr call_phi = make_edge_function(call_ef);

        // Look up the *current* (possibly updated) jump function for the
        // caller path edge (start_node, start_fact) -> (call, call_fact).
        // Using the stale incoming.caller_phi would produce wrong composed
        // edge functions whenever the jump function was updated after the
        // incoming edge was first recorded.
        PathEdgeType caller_edge(incoming.start_node, incoming.start_fact,
                                 incoming.call, incoming.call_fact);
        auto jf_it = m_jump_functions.find(caller_edge);
        EdgeFunctionPtr current_caller_phi = (jf_it != m_jump_functions.end())
                                             ? jf_it->second
                                             : identity_func;

        for (const llvm::Instruction* ret_site : get_return_sites(incoming.call)) {
            for (const auto& ret_fact : return_facts) {
                auto ret_ef = m_problem.return_edge_function(incoming.call, exit_fact, ret_fact);
                EdgeFunctionPtr ret_phi = make_edge_function(ret_ef);
                EdgeFunctionPtr composed = compose_cached(ret_phi,
                                          compose_cached(summary_phi,
                                          compose_cached(call_phi, current_caller_phi)));
                add_jump_function(PathEdgeType(incoming.start_node, incoming.start_fact,
                                               ret_site, ret_fact),
                                  composed);
            }
        }
    };

    // Initialize initial seeds
    auto seeds = m_problem.initial_seeds(module);
    if (seeds.empty()) {
        const llvm::Function* main_func = module.getFunction("main");
        if (!main_func) {
            for (const llvm::Function& f : module) {
                if (!f.isDeclaration() && !f.empty()) {
                    main_func = &f;
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
            add_jump_function(PathEdgeType(entry, fact, entry, fact), identity_func);
        }
    }

    // Phase 1: compute jump functions
    while (!m_worklist.empty()) {
        if (m_max_steps != 0 && m_steps_performed >= m_max_steps) {
            m_bound_reached = true;
            break;
        }

        auto work_item = m_worklist.back();
        m_worklist.pop_back();

        const PathEdgeType& edge = work_item.first;
        EdgeFunctionPtr phi = work_item.second;
        const llvm::Instruction* curr = edge.target_node;
        const Fact& start_fact = edge.start_fact;
        const Fact& fact = edge.target_fact;

        if (auto* call = llvm::dyn_cast<llvm::CallBase>(curr)) {
            auto it_callees = call_to_callees.find(call);
            std::vector<const llvm::Function*> callees;
            if (it_callees != call_to_callees.end()) {
                callees = it_callees->second;
            }
            if (callees.empty()) {
                callees.push_back(nullptr);
            }

            // Apply optional summary flow/edge functions (special-cased callees)
            for (const llvm::Function* callee : callees) {
                for (const llvm::Instruction* ret_site : get_return_sites(call)) {
                    FactSet summary_facts = m_problem.summary_flow(call, callee, fact);
                    preserve_zero(summary_facts, fact);
                    for (const auto& tgt_fact : summary_facts) {
                        auto ef = m_problem.summary_edge_function(call, fact, tgt_fact);
                        EdgeFunctionPtr edge_fn = make_edge_function(ef);
                        EdgeFunctionPtr new_phi = compose_cached(edge_fn, phi);
                        add_jump_function(PathEdgeType(edge.start_node, start_fact,
                                                       ret_site, tgt_fact),
                                          new_phi);
                    }
                }
            }

            // Always generate call-to-return edges
            FactSet ctr_facts = m_problem.call_to_return_flow(call, fact);
            preserve_zero(ctr_facts, fact);
            for (const llvm::Instruction* ret_site : get_return_sites(call)) {
                for (const auto& tgt_fact : ctr_facts) {
                    CallToReturnEdgeKey ckey(call, fact, tgt_fact);
                    auto eit = m_call_to_return_edge_cache.find(ckey);
                    EdgeFunctionPtr edge_fn;
                    if (eit != m_call_to_return_edge_cache.end()) {
                        edge_fn = eit->second;
                    } else {
                        edge_fn = make_edge_function(m_problem.call_to_return_edge_function(call, fact, tgt_fact));
                        m_call_to_return_edge_cache[ckey] = edge_fn;
                    }
                    EdgeFunctionPtr new_phi = compose_cached(edge_fn, phi);
                    add_jump_function(PathEdgeType(edge.start_node, start_fact,
                                                   ret_site, tgt_fact),
                                      new_phi);
                }
            }

            for (const llvm::Function* callee : callees) {
                if (!callee || callee->isDeclaration() || callee->empty()) {
                    continue;
                }
                const llvm::Instruction* callee_entry = &callee->getEntryBlock().front();
                FactSet call_facts = m_problem.call_flow(call, callee, fact);
                preserve_zero(call_facts, fact);
                for (const auto& callee_fact : call_facts) {
                    StartKey key{callee_entry, callee_fact};
                    IncomingEdge incoming{call, fact, edge.start_node, start_fact, phi};
                    add_incoming(key, incoming);

                    // Seed callee with identity jump function
                    add_jump_function(PathEdgeType(callee_entry, callee_fact,
                                                   callee_entry, callee_fact),
                                      identity_func);

                    // Apply existing summaries for this callee start
                    auto summary_it = m_end_summaries.find(key);
                    if (summary_it != m_end_summaries.end()) {
                        for (const auto& exit_pair : summary_it->second) {
                            const Fact& exit_fact = exit_pair.first;
                            for (const auto& summary_phi : exit_pair.second) {
                                apply_summary_to_incoming(incoming, callee, callee_fact,
                                                          exit_fact, summary_phi);
                            }
                        }
                    }
                }
            }

        } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(curr)) {
            const llvm::Function* func = ret->getFunction();
            if (!func || func->empty()) {
                continue;
            }
            const llvm::Instruction* entry = &func->getEntryBlock().front();
            StartKey key{entry, start_fact};

            if (add_summary(key, fact, phi)) {
                auto incoming_it = m_incoming.find(key);
                if (incoming_it != m_incoming.end()) {
                    for (const auto& incoming : incoming_it->second) {
                        apply_summary_to_incoming(incoming, func, start_fact, fact, phi);
                    }
                }
            }

            // Unbalanced returns: no incoming call edge for (entry, start_fact).
            // This handles the case where a function is analyzed as a seed
            // (i.e. its entry is a seed node) but is also called from elsewhere
            // in the program.  We propagate the return value to those call sites
            // using the zero fact as the caller context.
            if (m_config.follow_returns_past_seeds()) {
                auto incoming_it = m_incoming.find(key);
                if (incoming_it == m_incoming.end() || incoming_it->second.empty()) {
                    auto callee_calls_it = callee_to_calls.find(func);
                    if (callee_calls_it != callee_to_calls.end()) {
                        Fact zero_fact = m_problem.zero_fact();
                        for (const llvm::CallBase* call : callee_calls_it->second) {
                            FactSet return_facts = m_problem.return_flow(call, func, fact, zero_fact);
                            preserve_zero(return_facts, fact);
                            for (const llvm::Instruction* ret_site : get_return_sites(call)) {
                                for (const Fact& rf : return_facts) {
                                    auto ret_ef = m_problem.return_edge_function(call, fact, rf);
                                    EdgeFunctionPtr ret_phi = make_edge_function(ret_ef);
                                    EdgeFunctionPtr composed = compose_cached(ret_phi, phi);
                                    // BUG (fixed): the old code created a self-loop path edge
                                    // PathEdge(ret_site, zero_fact, ret_site, rf).  This seeds
                                    // a brand-new analysis context rooted at ret_site, which is
                                    // wrong: the return should connect back to the *callee's*
                                    // entry context (entry, start_fact), not start a new one at
                                    // the return site.  The correct start node is the callee
                                    // entry and the correct start fact is start_fact (the fact
                                    // that was live at the callee entry under the seed context).
                                    add_jump_function(PathEdgeType(entry, start_fact,
                                                                   ret_site, rf),
                                                      composed);
                                }
                            }
                        }
                    }
                }
            }

        } else {
            auto succ_it = successors.find(curr);
            if (succ_it != successors.end()) {
                for (const llvm::Instruction* succ : succ_it->second) {
                    FactSet next_facts = m_problem.normal_flow(curr, fact);
                    preserve_zero(next_facts, fact);
                    for (const auto& tgt_fact : next_facts) {
                        NormalEdgeKey nkey(curr, fact, tgt_fact);
                        auto eit = m_normal_edge_cache.find(nkey);
                        EdgeFunctionPtr edge_fn;
                        if (eit != m_normal_edge_cache.end()) {
                            edge_fn = eit->second;
                        } else {
                            edge_fn = make_edge_function(m_problem.normal_edge_function(curr, fact, tgt_fact));
                            m_normal_edge_cache[nkey] = edge_fn;
                        }
                        EdgeFunctionPtr new_phi = compose_cached(edge_fn, phi);
                        add_jump_function(PathEdgeType(edge.start_node, start_fact,
                                                       succ, tgt_fact),
                                          new_phi);
                    }
                }
            }
        }

        m_steps_performed++;
    }

    // Phase 2: compute values using jump functions
    struct ValueEdge {
        const llvm::Instruction* target_node;
        Fact target_fact;
        EdgeFunctionPtr phi;
    };

    std::unordered_map<StartKey, std::vector<ValueEdge>, StartKeyHash> value_edges;

    for (const auto& entry : m_jump_functions) {
        const PathEdgeType& edge = entry.first;
        const EdgeFunctionPtr& phi = entry.second;
        StartKey key{edge.start_node, edge.start_fact};
        value_edges[key].push_back(ValueEdge{edge.target_node, edge.target_fact, phi});
    }

    for (const auto& entry : m_incoming) {
        const StartKey& callee_key = entry.first;
        for (const auto& incoming : entry.second) {
            auto call_ef = m_problem.call_edge_function(incoming.call, incoming.call_fact, callee_key.start_fact);
            EdgeFunctionPtr call_phi = make_edge_function(call_ef);
            // BUG (fixed): the old code used incoming.caller_phi directly.
            // caller_phi is the jump-function value at the time the incoming
            // edge was first recorded.  If the jump function for the caller
            // path edge was later updated (joined with a new path), the stored
            // caller_phi is stale and composing with it produces wrong values.
            // We must look up the *current* jump function from m_jump_functions.
            PathEdgeType caller_edge(incoming.start_node, incoming.start_fact,
                                     incoming.call, incoming.call_fact);
            auto jf_it = m_jump_functions.find(caller_edge);
            EdgeFunctionPtr current_caller_phi = (jf_it != m_jump_functions.end())
                                                 ? jf_it->second
                                                 : identity_func;
            EdgeFunctionPtr composed = compose_cached(call_phi, current_caller_phi);
            StartKey caller_key{incoming.start_node, incoming.start_fact};
            value_edges[caller_key].push_back(ValueEdge{callee_key.start_node,
                                                        callee_key.start_fact,
                                                        composed});
        }
    }

    auto update_value = [&](const llvm::Instruction* inst, const Fact& fact, const Value& incoming_value) {
        auto& fact_map = m_values[inst];
        auto current_it = fact_map.find(fact);
        Value current = (current_it != fact_map.end()) ? current_it->second : m_problem.bottom_value();
        Value joined = m_problem.join(current, incoming_value);
        if (current_it != fact_map.end() && joined == current) {
            return false;
        }
        fact_map[fact] = joined;
        return true;
    };

    std::vector<StartKey> value_worklist;
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
            if (update_value(entry, fact, m_problem.top_value())) {
                value_worklist.push_back(StartKey{entry, fact});
            }
        }
    }

    while (!value_worklist.empty()) {
        StartKey key = value_worklist.back();
        value_worklist.pop_back();
        auto val_it = m_values.find(key.start_node);
        if (val_it == m_values.end()) {
            continue;
        }
        auto fact_it = val_it->second.find(key.start_fact);
        if (fact_it == val_it->second.end()) {
            continue;
        }
        const Value& start_value = fact_it->second;

        auto edge_it = value_edges.find(key);
        if (edge_it == value_edges.end()) {
            continue;
        }
        for (const auto& edge : edge_it->second) {
            Value result_val = (*edge.phi)(start_value);
            if (update_value(edge.target_node, edge.target_fact, result_val)) {
                value_worklist.push_back(StartKey{edge.target_node, edge.target_fact});
            }
        }
    }
}

template<typename Problem>
typename IDESolver<Problem>::Value
IDESolver<Problem>::get_value_at(const llvm::Instruction* inst, const typename Problem::FactType& fact) const {
    auto inst_it = m_values.find(inst);
    if (inst_it != m_values.end()) {
        auto fact_it = inst_it->second.find(fact);
        if (fact_it != inst_it->second.end()) {
            return fact_it->second;
        }
    }
    return m_problem.bottom_value();
}

template<typename Problem>
typename IDESolver<Problem>::Value
IDESolver<Problem>::get_value_at_in_llvm_ssa(const llvm::Instruction* inst, const typename Problem::FactType& fact) const {
    if (inst->getType()->isVoidTy()) {
        return get_value_at(inst, fact);
    }
    if (const llvm::Instruction* next = inst->getNextNode()) {
        return get_value_at(next, fact);
    }
    if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(inst)) {
        llvm::BasicBlock* normal = invoke->getNormalDest();
        if (normal && !normal->empty()) {
            return get_value_at(&normal->front(), fact);
        }
    }
    return get_value_at(inst, fact);
}

template<typename Problem>
const std::unordered_map<const llvm::Instruction*,
                        std::unordered_map<typename Problem::FactType, typename Problem::ValueType>>&
IDESolver<Problem>::get_all_values() const {
    return m_values;
}

} // namespace ifds
