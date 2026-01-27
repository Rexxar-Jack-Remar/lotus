#ifndef CHECKER_PULSE_PULSECONTRADICTION_H
#define CHECKER_PULSE_PULSECONTRADICTION_H

#include "Checker/Pulse/PulseAbstractValue.h"
#include <llvm/ADT/Optional.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class Value;
class Instruction;
} // namespace llvm

namespace pulse {

class PulseFormula;

/**
 * Contradiction types when applying summaries, following Infer's design.
 */
enum class ContradictionKind {
    None,
    Aliasing,                    // Distinct formals in pre are aliased in caller
    AliasingWithAllAliases,     // Aliasing with collected alias classes
    PathCondition,               // Path condition is UNSAT
    FormalActualLength,          // Mismatch in formal/actual argument counts
    CapturedFormalActualLength,  // Mismatch in captured formal/actual counts (for closures)
    DynamicTypeNeeded           // Dynamic type specialization needed
};

/**
 * Contradiction information for summary application.
 */
struct Contradiction {
    ContradictionKind kind;
    
    // For Aliasing contradiction
    struct {
        AbstractValue addr_caller;
        AbstractValue addr_callee;
        AbstractValue addr_callee_prime;
    } aliasing;
    
    // For AliasingWithAllAliases
    std::vector<std::vector<AbstractValue>> alias_classes;
    
    // For PathCondition
    std::string unsat_reason;
    
    // For FormalActualLength
    struct {
        unsigned formal_count;
        unsigned actual_count;
    } length_mismatch;
    
    // For CapturedFormalActualLength
    struct {
        unsigned captured_formal_count;
        unsigned captured_actual_count;
    } captured_length_mismatch;
    
    // For DynamicTypeNeeded
    std::map<AbstractValue, std::string> dynamic_type_paths;
    
    Contradiction() : kind(ContradictionKind::None) {}
    
    static Contradiction makeAliasing(AbstractValue caller, AbstractValue callee1, AbstractValue callee2) {
        Contradiction c;
        c.kind = ContradictionKind::Aliasing;
        c.aliasing.addr_caller = caller;
        c.aliasing.addr_callee = callee1;
        c.aliasing.addr_callee_prime = callee2;
        return c;
    }
    
    static Contradiction makePathCondition(const std::string& reason) {
        Contradiction c;
        c.kind = ContradictionKind::PathCondition;
        c.unsat_reason = reason;
        return c;
    }
    
    static Contradiction makeFormalActualLength(unsigned formal_count, unsigned actual_count) {
        Contradiction c;
        c.kind = ContradictionKind::FormalActualLength;
        c.length_mismatch.formal_count = formal_count;
        c.length_mismatch.actual_count = actual_count;
        return c;
    }
    
    static Contradiction makeCapturedFormalActualLength(unsigned captured_formal_count, 
                                                         unsigned captured_actual_count) {
        Contradiction c;
        c.kind = ContradictionKind::CapturedFormalActualLength;
        c.captured_length_mismatch.captured_formal_count = captured_formal_count;
        c.captured_length_mismatch.captured_actual_count = captured_actual_count;
        return c;
    }
    
    static Contradiction makeAliasingWithAllAliases(
        const std::vector<std::vector<AbstractValue>>& alias_classes) {
        Contradiction c;
        c.kind = ContradictionKind::AliasingWithAllAliases;
        c.alias_classes = alias_classes;
        return c;
    }
    
    static Contradiction makeDynamicTypeNeeded(
        const std::map<AbstractValue, std::string>& paths) {
        Contradiction c;
        c.kind = ContradictionKind::DynamicTypeNeeded;
        c.dynamic_type_paths = paths;
        return c;
    }
};

/**
 * Check for contradictions when applying a summary.
 * Returns None if no contradiction, Some(contradiction) if found.
 */
llvm::Optional<Contradiction> checkContradiction(
    const PulseFormula& caller_formula,
    const PulseFormula& callee_pre_formula,
    const std::map<AbstractValue, AbstractValue>& formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>& actual_to_formals_map);

/**
 * Check for aliasing contradictions: distinct formals in pre that map to same actual.
 */
llvm::Optional<Contradiction> checkAliasingContradiction(
    const std::map<AbstractValue, AbstractValue>& formal_to_actual_map,
    const PulseFormula& callee_pre_formula);

/**
 * Check if merged formulas are UNSAT (path condition contradiction).
 */
llvm::Optional<Contradiction> checkPathConditionContradiction(
    const PulseFormula& caller_formula,
    const PulseFormula& callee_pre_formula);

/**
 * Check for AliasingWithAllAliases contradiction
 * Collects all alias classes before raising contradiction
 */
llvm::Optional<Contradiction> checkAliasingWithAllAliases(
    const std::map<AbstractValue, AbstractValue>& formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>& actual_to_formals_map,
    const PulseFormula& callee_pre_formula);

/**
 * Check for DynamicTypeNeeded contradiction
 * Returns map of abstract values to heap paths that need dynamic type specialization
 */
llvm::Optional<Contradiction> checkDynamicTypeNeeded(
    const std::map<AbstractValue, std::string>& heap_paths_to_values);

/**
 * Check for CapturedFormalActualLength contradiction
 */
llvm::Optional<Contradiction> checkCapturedFormalActualLength(
    unsigned captured_formal_count,
    unsigned captured_actual_count);

} // namespace pulse

#endif // CHECKER_PULSE_PULSECONTRADICTION_H
