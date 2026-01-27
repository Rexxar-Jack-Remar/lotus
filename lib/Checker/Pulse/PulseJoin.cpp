#include "Checker/Pulse/PulseJoin.h"
#include "Checker/Pulse/PulseLogger.h"
#include "Checker/Pulse/PulseOperations.h"
#include "Checker/Pulse/PulsePathContext.h"
#include <algorithm>
#include <limits>

namespace pulse {

//===----------------------------------------------------------------------===//
// JoinState Helper Methods
//===----------------------------------------------------------------------===//

static ValueHistory joinHistories(const ValueHistory& hist1, const ValueHistory& hist2) {
    // Compare histories by checking if they're empty or have same events
    // Simplified comparison - full implementation would do deep comparison
    if (hist1.isEmpty() && hist2.isEmpty()) {
        return hist1;
    }
    if (hist1.isEmpty()) {
        return hist2;
    }
    if (hist2.isEmpty()) {
        return hist1;
    }
    // For now, return epoch (simplified - full implementation would merge histories)
    return ValueHistory();  // Empty history (epoch)
}

static ValueHistory joinHistoriesOpts(llvm::Optional<ValueHistory> hist1_opt,
                                      llvm::Optional<ValueHistory> hist2_opt) {
    if (hist1_opt && hist2_opt) {
        return joinHistories(*hist1_opt, *hist2_opt);
    }
    // If only one side has history, return epoch
    return ValueHistory();
}

//===----------------------------------------------------------------------===//
// Value Joining
//===----------------------------------------------------------------------===//

std::pair<PulseJoin::JoinState&, std::pair<AbstractValue, ValueHistory>>
PulseJoin::joinValuesHists(JoinState& state,
                          const AbstractValue& lhs_val, const ValueHistory& lhs_hist,
                          const AbstractValue& rhs_val, const ValueHistory& rhs_hist) {
    using Key = std::pair<llvm::Optional<AbstractValue>, llvm::Optional<AbstractValue>>;
    
    if (lhs_val == rhs_val) {
        // Same value: x↦v ⊔ x↦v = x↦v
        Key key{llvm::Optional<AbstractValue>(lhs_val), llvm::Optional<AbstractValue>(rhs_val)};
        auto& rev_subst = state.rev_subst;
        rev_subst[lhs_val] = key;
        ValueHistory hist_join = joinHistories(lhs_hist, rhs_hist);
        return {state, {lhs_val, hist_join}};
    }
    
    // Different values: x↦v ⊔ x↦v' = x↦v''
    Key key{llvm::Optional<AbstractValue>(lhs_val), llvm::Optional<AbstractValue>(rhs_val)};
    auto it = state.subst.find(key);
    if (it != state.subst.end()) {
        // Already joined this pair
        ValueHistory hist_join = joinHistories(lhs_hist, rhs_hist);
        return {state, {it->second, hist_join}};
    }
    
    // Create fresh joined value
    AbstractValue v_join = state.factory->createFresh();
    state.subst[key] = v_join;
    state.rev_subst[v_join] = key;
    
    ValueHistory hist_join = joinHistories(lhs_hist, rhs_hist);
    return {state, {v_join, hist_join}};
}

std::pair<PulseJoin::JoinState&, std::pair<AbstractValue, ValueHistory>>
PulseJoin::joinValuesHistsOpts(JoinState& state,
                               const AbductiveDomain& lhs_astate,
                               llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt,
                               const AbductiveDomain& rhs_astate,
                               llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt) {
    using Key = std::pair<llvm::Optional<AbstractValue>, llvm::Optional<AbstractValue>>;
    
    if (!lhs_opt && !rhs_opt) {
        // Both empty - shouldn't happen
        assert(false && "Both sides empty in join");
        AbstractValue v_join = state.factory->createFresh();
        return {state, {v_join, ValueHistory()}};
    }
    
    if (!lhs_opt || !rhs_opt) {
        // One-sided: x↦v ⊔ emp = x↦v' (v' fresh)
        AbstractValue v_join = state.factory->createFresh();
        ValueHistory hist = lhs_opt ? lhs_opt->second : rhs_opt->second;
        ValueHistory hist_join = joinHistoriesOpts(
            lhs_opt ? llvm::Optional<ValueHistory>(lhs_opt->second) : llvm::None,
            rhs_opt ? llvm::Optional<ValueHistory>(rhs_opt->second) : llvm::None);
        return {state, {v_join, hist_join}};
    }
    
    // Both sides have values
    if (lhs_opt->first == rhs_opt->first) {
        // Same value: x↦v ⊔ x↦v = x↦v
        Key key{llvm::Optional<AbstractValue>(lhs_opt->first), llvm::Optional<AbstractValue>(rhs_opt->first)};
        state.rev_subst[lhs_opt->first] = key;
        ValueHistory hist_join = joinHistories(lhs_opt->second, rhs_opt->second);
        return {state, {lhs_opt->first, hist_join}};
    }
    
    // Different values: use cached join
    Key key{llvm::Optional<AbstractValue>(lhs_opt->first), llvm::Optional<AbstractValue>(rhs_opt->first)};
    auto it = state.subst.find(key);
    if (it != state.subst.end()) {
        ValueHistory hist_join = joinHistories(lhs_opt->second, rhs_opt->second);
        return {state, {it->second, hist_join}};
    }
    
    return joinValuesHists(state, lhs_opt->first, lhs_opt->second,
                          rhs_opt->first, rhs_opt->second);
}

//===----------------------------------------------------------------------===//
// Heap Joining
//===----------------------------------------------------------------------===//

std::pair<PulseJoin::JoinState&, Heap>
PulseJoin::joinHeaps(JoinState& state, Heap& heap_join,
                     const AbductiveDomain& lhs_astate,
                     llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt,
                     const AbductiveDomain& rhs_astate,
                     llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt) {
    using Key = std::pair<llvm::Optional<AbstractValue>, llvm::Optional<AbstractValue>>;
    
    if (!lhs_opt && !rhs_opt) {
        return {state, heap_join};
    }
    
    Key visited_key{lhs_opt ? llvm::Optional<AbstractValue>(lhs_opt->first) : llvm::None,
                    rhs_opt ? llvm::Optional<AbstractValue>(rhs_opt->first) : llvm::None};
    
    if (state.visited.count(visited_key) > 0) {
        return {state, heap_join};  // Already visited
    }
    state.visited.insert(visited_key);
    
    // Join the values first
    auto value_result = joinValuesHistsOpts(state, lhs_astate, lhs_opt, rhs_astate, rhs_opt);
    state = value_result.first;
    auto v_hist_join = value_result.second;
    
    // Collect edges from both sides
    std::set<Access> all_accesses;
    if (lhs_opt) {
        const auto& lhs_edges = lhs_astate.getPostHeap().getEdges();
        auto lhs_it = lhs_edges.find(lhs_opt->first);
        if (lhs_it != lhs_edges.end()) {
            for (const auto& edge_kv : lhs_it->second) {
                all_accesses.insert(edge_kv.first);
            }
        }
    }
    if (rhs_opt) {
        const auto& rhs_edges = rhs_astate.getPostHeap().getEdges();
        auto rhs_it = rhs_edges.find(rhs_opt->first);
        if (rhs_it != rhs_edges.end()) {
            for (const auto& edge_kv : rhs_it->second) {
                all_accesses.insert(edge_kv.first);
            }
        }
    }
    
    // Join edges
    for (const Access& access : all_accesses) {
        llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_target_opt;
        llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_target_opt;
        
        if (lhs_opt) {
            const auto* lhs_target = lhs_astate.getPostHeap().findEdge(lhs_opt->first, access);
            if (lhs_target) {
                lhs_target_opt = {{lhs_target->addr, lhs_target->history}};
            }
        }
        if (rhs_opt) {
            const auto* rhs_target = rhs_astate.getPostHeap().findEdge(rhs_opt->first, access);
            if (rhs_target) {
                rhs_target_opt = {{rhs_target->addr, rhs_target->history}};
            }
        }
        
        // Recursively join target values
        auto target_result = joinValuesHistsOpts(state, lhs_astate, lhs_target_opt,
                                                  rhs_astate, rhs_target_opt);
        state = target_result.first;
        auto target_v_hist = target_result.second;
        
        // Recursively join heaps from targets
        auto heap_result = joinHeaps(state, heap_join, lhs_astate, lhs_target_opt,
                                     rhs_astate, rhs_target_opt);
        state = heap_result.first;
        heap_join = heap_result.second;
        
        // Add edge to joined heap
        Address target_addr(target_v_hist.first);
        target_addr.history = target_v_hist.second;
        heap_join.addEdge(v_hist_join.first, access, target_addr);
    }
    
    return {state, heap_join};
}

//===----------------------------------------------------------------------===//
// Stack Joining
//===----------------------------------------------------------------------===//

std::pair<PulseJoin::JoinState&, std::pair<Stack, Heap>>
PulseJoin::joinStacks(JoinState& state,
                      const AbductiveDomain& lhs_astate,
                      const AbductiveDomain& rhs_astate) {
    Stack stack_pre_join, stack_post_join;
    Heap heap_pre_join, heap_post_join;
    
    // Collect all variables from both sides
    std::set<const llvm::Value*> all_vars;
    for (const auto& kv : lhs_astate.getPostStack().getMap()) {
        all_vars.insert(kv.first);
    }
    for (const auto& kv : rhs_astate.getPostStack().getMap()) {
        all_vars.insert(kv.first);
    }
    
    // Join post stack
    for (const llvm::Value* var : all_vars) {
        const Address* lhs_addr = lhs_astate.getPostStack().find(var);
        const Address* rhs_addr = rhs_astate.getPostStack().find(var);
        
        llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt;
        llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt;
        
        if (lhs_addr) {
            lhs_opt = {{lhs_addr->addr, lhs_addr->history}};
        }
        if (rhs_addr) {
            rhs_opt = {{rhs_addr->addr, rhs_addr->history}};
        }
        
        // Join values
        auto result = joinValuesHistsOpts(state, lhs_astate, lhs_opt, rhs_astate, rhs_opt);
        state = result.first;
        auto v_hist_join = result.second;
        
        // Join heaps from stack values
        auto heap_result = joinHeaps(state, heap_post_join, lhs_astate, lhs_opt, rhs_astate, rhs_opt);
        state = heap_result.first;
        heap_post_join = heap_result.second;
        
        // Add to joined stack
        Address joined_addr(v_hist_join.first);
        joined_addr.history = v_hist_join.second;
        stack_post_join.add(var, joined_addr);
    }
    
    // Join pre stack similarly (simplified - full implementation would handle pre separately)
    for (const llvm::Value* var : all_vars) {
        const Address* lhs_addr = lhs_astate.getPreStack().find(var);
        const Address* rhs_addr = rhs_astate.getPreStack().find(var);
        
        if (lhs_addr || rhs_addr) {
            llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt;
            llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt;
            
            if (lhs_addr) {
                lhs_opt = {{lhs_addr->addr, lhs_addr->history}};
            }
            if (rhs_addr) {
                rhs_opt = {{rhs_addr->addr, rhs_addr->history}};
            }
            
            auto pre_result = joinValuesHistsOpts(state, lhs_astate, lhs_opt, rhs_astate, rhs_opt);
            state = pre_result.first;
            auto v_hist_join = pre_result.second;
            
            Address joined_addr(v_hist_join.first);
            joined_addr.history = v_hist_join.second;
            stack_pre_join.add(var, joined_addr);
        }
    }
    
    return {state, {stack_pre_join, heap_pre_join}};
}

//===----------------------------------------------------------------------===//
// Attribute Joining
//===----------------------------------------------------------------------===//

llvm::Optional<Attribute> PulseJoin::joinOneSidedAttribute(Attribute attr) {
    // Some attributes are kept even if only one-sided
    switch (attr) {
        case Attribute::Allocated:
            return attr;  // Keep allocated
        case Attribute::Invalid:
        case Attribute::Null:
        case Attribute::Uninitialized:
            return llvm::None;  // Drop these (too strong)
        case Attribute::Tainted:
            return llvm::None;  // Drop (conservative)
        case Attribute::FileHandle:
        case Attribute::Lock:
        case Attribute::AsyncResource:
            return llvm::None;  // Drop resource attributes (too strong)
        default:
            return llvm::None;
    }
}

llvm::Optional<Attribute> PulseJoin::joinTwoSidedAttribute(JoinState& state,
                                                           Attribute attr1,
                                                           Attribute attr2,
                                                           AbstractValue lhs_val,
                                                           AbstractValue rhs_val) {
    if (attr1 == attr2) {
        return attr1;  // Same attribute
    }
    
    // Different attributes - check if compatible
    if (attr1 == Attribute::Allocated && attr2 == Attribute::Allocated) {
        return Attribute::Allocated;
    }
    
    // Incompatible attributes - return None
    return llvm::None;
}

AttributeSet PulseJoin::joinAttributes(JoinState& state,
                                      const AbductiveDomain& lhs_astate,
                                      const AbductiveDomain& rhs_astate,
                                      AbstractValue joined_addr,
                                      llvm::Optional<AbstractValue> lhs_addr_opt,
                                      llvm::Optional<AbstractValue> rhs_addr_opt) {
    AttributeSet result;
    
    AttributeSet lhs_attrs;
    AttributeSet rhs_attrs;
    
    if (lhs_addr_opt) {
        lhs_attrs = lhs_astate.getPostAttrs().get(*lhs_addr_opt);
    }
    if (rhs_addr_opt) {
        rhs_attrs = rhs_astate.getPostAttrs().get(*rhs_addr_opt);
    }
    
    // Collect all attributes
    std::set<Attribute> all_attrs;
    for (Attribute attr : lhs_attrs) {
        all_attrs.insert(attr);
    }
    for (Attribute attr : rhs_attrs) {
        all_attrs.insert(attr);
    }
    
    // Join each attribute
    for (Attribute attr : all_attrs) {
        bool in_lhs = lhs_attrs.count(attr) > 0;
        bool in_rhs = rhs_attrs.count(attr) > 0;
        
        llvm::Optional<Attribute> joined_attr;
        if (in_lhs && in_rhs) {
            // Two-sided
            if (lhs_addr_opt && rhs_addr_opt) {
                joined_attr = joinTwoSidedAttribute(state, attr, attr, *lhs_addr_opt, *rhs_addr_opt);
            } else {
                joined_attr = attr;  // Same attribute on both sides
            }
        } else {
            // One-sided
            joined_attr = joinOneSidedAttribute(attr);
        }
        
        if (joined_attr) {
            result.insert(*joined_attr);
        }
    }
    
    return result;
}

//===----------------------------------------------------------------------===//
// Formula Joining
//===----------------------------------------------------------------------===//

PulseFormula PulseJoin::joinFormulas(const AbductiveDomain& lhs, const AbductiveDomain& rhs) {
    const PulseFormula& lhs_formula = lhs.getPathFormula();
    const PulseFormula& rhs_formula = rhs.getPathFormula();
    return PulseFormula::merge(lhs_formula, rhs_formula);
}

//===----------------------------------------------------------------------===//
// Main Join Operations
//===----------------------------------------------------------------------===//

llvm::Optional<AbductiveDomain>
PulseJoin::joinAbductive(const AbductiveDomain& lhs, const AbductiveDomain& rhs) {
    PulseLogger::trace("Joining abductive domains");
    PulseLogger::incrementCounter("joins.performed");
    
    // Check formula consistency first
    PulseFormula merged_formula = joinFormulas(lhs, rhs);
    if (!merged_formula.isConsistent() || merged_formula.isUnsat()) {
        PulseLogger::debug("Join failed: formula contradiction");
        PulseLogger::incrementCounter("joins.failed");
        return llvm::None;  // Contradiction
    }
    
    // Use the existing merge implementation
    // merge returns Optional<AbductiveDomain> which we can return directly
    auto merged_opt = AbductiveDomain::merge(lhs, rhs);
    if (!merged_opt) {
        return llvm::None;
    }
    
    // AbductiveDomain::merge now handles all additional fields:
    // - TransitiveInfo.join
    // - Loop header info (preserved from lhs)
    // - Unknown values flag (OR of both)
    // - Skipped calls (union)
    // - Dynamic type specialization needs (union)
    // - Recursive calls (union)
    // - Loop invariant under inference (preserved from lhs)
    
    // Move the merged domain to avoid copy
    return std::move(*merged_opt);
}

llvm::Optional<std::pair<AbductiveDomain, PathContext>>
PulseJoin::join(const AbductiveDomain& lhs, const PathContext& path_lhs,
                const AbductiveDomain& rhs, const PathContext& path_rhs) {
    auto joined_domain_opt = joinAbductive(lhs, rhs);
    if (!joined_domain_opt) {
        return llvm::None;
    }
    
    PathContext joined_path = PathContext::join(path_lhs, path_rhs);
    // Use std::make_pair to avoid copy constructor issues
    return llvm::Optional<std::pair<AbductiveDomain, PathContext>>(
        std::make_pair(std::move(*joined_domain_opt), joined_path));
}

AbductiveDomain PulseJoin::joinSummaries(const AbductiveDomain& lhs, const AbductiveDomain& rhs) {
    auto joined_opt = joinAbductive(lhs, rhs);
    if (!joined_opt) {
        // If join fails, return empty domain (or could return lhs as fallback)
        return AbductiveDomain();
    }
    // Move the result to avoid copy constructor
    return std::move(*joined_opt);
}

} // namespace pulse
