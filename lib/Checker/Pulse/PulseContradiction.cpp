#include "Checker/Pulse/PulseContradiction.h"
#include "Checker/Pulse/PulseFormula.h"
#include <algorithm>

namespace pulse {

llvm::Optional<Contradiction> checkAliasingContradiction(
    const std::map<AbstractValue, AbstractValue>& formal_to_actual_map,
    const PulseFormula& callee_pre_formula) {
    
    // Build reverse map: actual -> set of formals that map to it
    std::map<AbstractValue, std::set<AbstractValue>> actual_to_formals;
    for (const auto& kv : formal_to_actual_map) {
        AbstractValue formal = kv.first;
        AbstractValue actual = kv.second;
        actual_to_formals[actual].insert(formal);
    }
    
    // Check if any actual maps to multiple distinct formals
    for (const auto& kv : actual_to_formals) {
        const std::set<AbstractValue>& formals = kv.second;
        if (formals.size() < 2) {
            continue;  // Only one formal maps to this actual
        }
        
        // Check if these formals are known to be distinct in the callee's pre-condition
        std::vector<AbstractValue> formal_vec(formals.begin(), formals.end());
        for (size_t i = 0; i < formal_vec.size(); ++i) {
            for (size_t j = i + 1; j < formal_vec.size(); ++j) {
                AbstractValue formal1 = formal_vec[i];
                AbstractValue formal2 = formal_vec[j];
                
                // Check if they're known to be equal in pre (if so, no contradiction)
                if (callee_pre_formula.areEqual(formal1, formal2)) {
                    continue;
                }
                
                // Check if they're known to be disequal in pre (contradiction!)
                if (callee_pre_formula.areDisequal(formal1, formal2)) {
                    return Contradiction::makeAliasing(kv.first, formal1, formal2);
                }
                
                // If pre doesn't explicitly say they're equal, and they're distinct abstract values,
                // we assume they're distinct (conservative: may be false positive but safe)
                if (!(formal1 == formal2)) {
                    // This is a potential aliasing contradiction
                    // In a more precise implementation, we'd check heap paths
                    return Contradiction::makeAliasing(kv.first, formal1, formal2);
                }
            }
        }
    }
    
    return llvm::None;
}

llvm::Optional<Contradiction> checkPathConditionContradiction(
    const PulseFormula& caller_formula,
    const PulseFormula& callee_pre_formula) {
    
    // Try to merge formulas - if merge fails, we have a contradiction
    PulseFormula merged = PulseFormula::merge(caller_formula, callee_pre_formula);
    if (!merged.isConsistent()) {
        return Contradiction::makePathCondition("Merged formula is inconsistent");
    }
    
    // Check for specific contradictions:
    // 1. Null vs non-null contradictions
    // 2. Equality vs disequality contradictions
    
    // This is a simplified check - full implementation would use SMT solver
    // For now, we rely on isConsistent() which checks basic contradictions
    
    return llvm::None;
}

llvm::Optional<Contradiction> checkContradiction(
    const PulseFormula& caller_formula,
    const PulseFormula& callee_pre_formula,
    const std::map<AbstractValue, AbstractValue>& formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>& actual_to_formals_map) {
    
    // Check aliasing contradiction (matching Infer's Aliasing and AliasingWithAllAliases)
    auto aliasing_contradiction = checkAliasingContradiction(formal_to_actual_map, callee_pre_formula);
    if (aliasing_contradiction) {
        return aliasing_contradiction;
    }
    
    // Check path condition contradiction (matching Infer's PathCondition)
    auto path_contradiction = checkPathConditionContradiction(caller_formula, callee_pre_formula);
    if (path_contradiction) {
        return path_contradiction;
    }
    
    return llvm::None;
}

/**
 * Check for AliasingWithAllAliases contradiction
 * Collects all alias classes before raising contradiction
 */
llvm::Optional<Contradiction> checkAliasingWithAllAliases(
    const std::map<AbstractValue, AbstractValue>& formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>& actual_to_formals_map,
    const PulseFormula& callee_pre_formula) {
    
    // Build alias classes: groups of formals that map to the same actual
    std::vector<std::vector<AbstractValue>> alias_classes;
    std::set<AbstractValue> processed;
    
    for (const auto& kv : actual_to_formals_map) {
        const std::set<AbstractValue>& formals = kv.second;
        if (formals.size() < 2) {
            continue;  // Only one formal maps to this actual
        }
        
        // Check if these formals are known to be distinct in pre
        std::vector<AbstractValue> formal_vec(formals.begin(), formals.end());
        bool all_distinct = true;
        
        for (size_t i = 0; i < formal_vec.size(); ++i) {
            for (size_t j = i + 1; j < formal_vec.size(); ++j) {
                AbstractValue formal1 = formal_vec[i];
                AbstractValue formal2 = formal_vec[j];
                
                // If they're equal in pre, not distinct
                if (callee_pre_formula.areEqual(formal1, formal2)) {
                    all_distinct = false;
                    break;
                }
            }
            if (!all_distinct) break;
        }
        
        // If all are distinct in pre but map to same actual, this is an alias class
        if (all_distinct) {
            bool any_new = false;
            for (AbstractValue f : formal_vec) {
                if (processed.count(f) == 0) {
                    any_new = true;
                    processed.insert(f);
                }
            }
            if (any_new) {
                alias_classes.push_back(formal_vec);
            }
        }
    }
    
    if (!alias_classes.empty()) {
        return Contradiction::makeAliasingWithAllAliases(alias_classes);
    }
    
    return llvm::None;
}

/**
 * Check for DynamicTypeNeeded contradiction
 * Returns map of abstract values to heap paths that need dynamic type specialization
 */
llvm::Optional<Contradiction> checkDynamicTypeNeeded(
    const std::map<AbstractValue, std::string>& heap_paths_to_values) {
    
    if (!heap_paths_to_values.empty()) {
        return Contradiction::makeDynamicTypeNeeded(heap_paths_to_values);
    }
    
    return llvm::None;
}

/**
 * Check for CapturedFormalActualLength contradiction
 */
llvm::Optional<Contradiction> checkCapturedFormalActualLength(
    unsigned captured_formal_count,
    unsigned captured_actual_count) {
    
    if (captured_formal_count != captured_actual_count) {
        return Contradiction::makeCapturedFormalActualLength(
            captured_formal_count, captured_actual_count);
    }
    
    return llvm::None;
}

} // namespace pulse
