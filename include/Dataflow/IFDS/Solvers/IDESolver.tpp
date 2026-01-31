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
void IDESolver<Problem>::solve(const llvm::Module& module) {
    using Fact = typename Problem::FactType;
    using Value = typename Problem::ValueType;

    // Clear previous results and caches
    m_values.clear();
    m_jump_functions.clear();
    m_incoming.clear();
    m_end_summaries.clear();
    m_compose_cache.clear();
    m_worklist.clear();

    // Build call graph
    std::unordered_map<const llvm::CallInst*, const llvm::Function*> call_to_callee;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> callee_to_calls;

    for (const llvm::Function& func : module) {
        if (func.isDeclaration()) continue;
        for (const llvm::BasicBlock& bb : func) {
            for (const llvm::Instruction& inst : bb) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (const llvm::Function* callee = call->getCalledFunction()) {
                        call_to_callee[call] = callee;
                        callee_to_calls[callee].push_back(call);
                    }
                }
            }
        }
    }

    // Build CFG successors (including switch and invoke)
    std::unordered_map<const llvm::Instruction*, std::vector<const llvm::Instruction*>> successors;
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
                } else if (llvm::isa<llvm::ReturnInst>(inst) ||
                          llvm::isa<llvm::UnreachableInst>(inst)) {
                    // No intraprocedural successors
                } else if (const llvm::Instruction* next = inst.getNextNode()) {
                    succs.push_back(next);
                }
                successors[&inst] = std::move(succs);
            }
        }
    }

    auto get_return_site = [](const llvm::CallInst* call) -> const llvm::Instruction* {
        if (!call->isTerminator()) {
            return call->getNextNode();
        }
        if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
            llvm::BasicBlock* normalDest = invoke->getNormalDest();
            if (normalDest && !normalDest->empty()) {
                return &normalDest->front();
            }
        }
        return nullptr;
    };

    auto preserve_zero = [&](FactSet& facts, const Fact& source_fact) {
        if (m_problem.auto_add_zero() && m_problem.is_zero_fact(source_fact)) {
            facts.insert(m_problem.zero_fact());
        }
    };

    EdgeFunctionPtr identity_func = make_edge_function(m_problem.identity());

    auto add_jump_function = [&](const PathEdgeType& edge, EdgeFunctionPtr phi) {
        auto& funcs = m_jump_functions[edge];
        if (std::find(funcs.begin(), funcs.end(), phi) != funcs.end()) {
            return;
        }
        funcs.push_back(phi);
        m_worklist.emplace_back(edge, phi);
    };

    auto add_incoming = [&](const StartKey& key, const IncomingEdge& incoming) {
        auto& list = m_incoming[key];
        auto it = std::find(list.begin(), list.end(), incoming);
        if (it == list.end()) {
            list.push_back(incoming);
        }
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
        const llvm::Instruction* ret_site = get_return_site(incoming.call);
        if (!ret_site) {
            return;
        }

        FactSet return_facts = m_problem.return_flow(incoming.call, callee, exit_fact, incoming.call_fact);
        preserve_zero(return_facts, exit_fact);

        auto call_ef = m_problem.call_edge_function(incoming.call, incoming.call_fact, callee_fact);
        EdgeFunctionPtr call_phi = make_edge_function(call_ef);

        for (const auto& ret_fact : return_facts) {
            auto ret_ef = m_problem.return_edge_function(incoming.call, exit_fact, ret_fact);
            EdgeFunctionPtr ret_phi = make_edge_function(ret_ef);
            EdgeFunctionPtr composed = compose_cached(ret_phi,
                                      compose_cached(summary_phi,
                                      compose_cached(call_phi, incoming.caller_phi)));
            add_jump_function(PathEdgeType(incoming.start_node, incoming.start_fact,
                                           ret_site, ret_fact),
                              composed);
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
        auto work_item = m_worklist.back();
        m_worklist.pop_back();

        const PathEdgeType& edge = work_item.first;
        EdgeFunctionPtr phi = work_item.second;
        const llvm::Instruction* curr = edge.target_node;
        const Fact& start_fact = edge.start_fact;
        const Fact& fact = edge.target_fact;

        if (auto* call = llvm::dyn_cast<llvm::CallInst>(curr)) {
            auto it_callee = call_to_callee.find(call);
            const llvm::Function* callee = (it_callee != call_to_callee.end()) ? it_callee->second : nullptr;

            // Apply optional summary flow/edge functions (special-cased callees)
            const llvm::Instruction* ret_site = get_return_site(call);
            if (ret_site) {
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

            // Always generate call-to-return edges
            ret_site = get_return_site(call);
            if (ret_site) {
                FactSet ctr_facts = m_problem.call_to_return_flow(call, fact);
                preserve_zero(ctr_facts, fact);
                for (const auto& tgt_fact : ctr_facts) {
                    auto ef = m_problem.call_to_return_edge_function(call, fact, tgt_fact);
                    EdgeFunctionPtr edge_fn = make_edge_function(ef);
                    EdgeFunctionPtr new_phi = compose_cached(edge_fn, phi);
                    add_jump_function(PathEdgeType(edge.start_node, start_fact,
                                                   ret_site, tgt_fact),
                                      new_phi);
                }
            }

            if (callee && !callee->isDeclaration() && !callee->empty()) {
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

        } else {
            auto succ_it = successors.find(curr);
            if (succ_it != successors.end()) {
                for (const llvm::Instruction* succ : succ_it->second) {
                    FactSet next_facts = m_problem.normal_flow(curr, fact);
                    preserve_zero(next_facts, fact);
                    for (const auto& tgt_fact : next_facts) {
                        auto ef = m_problem.normal_edge_function(curr, fact, tgt_fact);
                        EdgeFunctionPtr edge_fn = make_edge_function(ef);
                        EdgeFunctionPtr new_phi = compose_cached(edge_fn, phi);
                        add_jump_function(PathEdgeType(edge.start_node, start_fact,
                                                       succ, tgt_fact),
                                          new_phi);
                    }
                }
            }
        }
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
        for (const auto& phi : entry.second) {
            StartKey key{edge.start_node, edge.start_fact};
            value_edges[key].push_back(ValueEdge{edge.target_node, edge.target_fact, phi});
        }
    }

    for (const auto& entry : m_incoming) {
        const StartKey& callee_key = entry.first;
        for (const auto& incoming : entry.second) {
            auto call_ef = m_problem.call_edge_function(incoming.call, incoming.call_fact, callee_key.start_fact);
            EdgeFunctionPtr call_phi = make_edge_function(call_ef);
            EdgeFunctionPtr composed = compose_cached(call_phi, incoming.caller_phi);
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
const std::unordered_map<const llvm::Instruction*,
                        std::unordered_map<typename Problem::FactType, typename Problem::ValueType>>&
IDESolver<Problem>::get_all_values() const {
    return m_values;
}

} // namespace ifds
