#ifndef CHECKER_PULSE_PULSETAINT_H
#define CHECKER_PULSE_PULSETAINT_H

#include "Checker/Pulse/PulseAbstractValue.h"
#include "Checker/Pulse/PulseValueHistory.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pulse {

class AbductiveDomain;

/**
 * Taint kind: category of taint source
 */
enum class TaintKind {
    Unknown,
    UserInput,      // Data from user (CLI, etc.)
    Network,        // Data from network
    FileSystem,     // Data from file system
    Environment,    // Environment variables
    Sensitive       // Sensitive data (passwords, keys)
};

/**
 * Taint item: represents a specific instance of taint on a value
 * Production-ready implementation aligned with Infer's TaintItem
 */
struct TaintItem {
    TaintKind kind;
    const llvm::Instruction* source_instruction;
    const llvm::Function* source_function;
    ValueHistory history;  // How the taint propagated to current value
    unsigned timestamp;  // When taint was introduced
    bool intra_procedural_only;  // True if taint is only intra-procedural
    
    // Sanitizer information
    struct Sanitizer {
        TaintKind sanitizer_kind;
        const llvm::Instruction* sanitizer_location;
        unsigned sanitizer_timestamp;
        
        Sanitizer(TaintKind k, const llvm::Instruction* loc, unsigned ts)
            : sanitizer_kind(k), sanitizer_location(loc), sanitizer_timestamp(ts) {}
    };
    std::vector<Sanitizer> sanitizers;  // Sanitizers that have been applied

    TaintItem(TaintKind k, const llvm::Instruction* inst, const llvm::Function* func, unsigned ts = 0)
        : kind(k), source_instruction(inst), source_function(func), timestamp(ts), 
          intra_procedural_only(false) {}

    bool operator<(const TaintItem& other) const {
        if (kind != other.kind) return kind < other.kind;
        if (source_instruction != other.source_instruction) 
            return source_instruction < other.source_instruction;
        if (timestamp != other.timestamp) return timestamp < other.timestamp;
        return false;
    }
    
    /**
     * Check if this taint item is sanitized by any sanitizer
     */
    bool isSanitized() const {
        return !sanitizers.empty();
    }
    
    /**
     * Add a sanitizer to this taint item
     */
    void addSanitizer(TaintKind sanitizer_kind, const llvm::Instruction* loc, unsigned ts) {
        sanitizers.emplace_back(sanitizer_kind, loc, ts);
    }
};

/**
 * Taint domain: maps abstract values to their taint items
 */
class TaintDomain {
private:
    std::map<AbstractValue, std::set<TaintItem>> taints_;

public:
    void add(AbstractValue v, TaintItem item);
    void remove(AbstractValue v);
    bool has(AbstractValue v) const;
    const std::set<TaintItem>& get(AbstractValue v) const;
    
    // Merge two taint domains
    void join(const TaintDomain& other);
    
    const std::map<AbstractValue, std::set<TaintItem>>& getMap() const { return taints_; }
};

/**
 * Taint operations: high-level taint analysis logic
 * Production-ready implementation aligned with Infer's PulseTaintOperations
 */
class TaintOperations {
private:
    static unsigned global_timestamp_;  // Global timestamp counter
    
public:
    /**
     * Mark a value as tainted
     */
    static void taint(AbductiveDomain& astate,
                      AbstractValue v,
                      TaintKind kind,
                      const llvm::Instruction* source);

    /**
     * Check if a value is tainted and report if it flows to a sink
     * Returns true if a bug was reported
     */
    static bool checkSink(AbductiveDomain& astate,
                          AbstractValue v,
                          const std::string& sink_name,
                          const llvm::Instruction* sink_loc);

    /**
     * Propagate taint from source value to dest value
     */
    static void propagate(AbductiveDomain& astate,
                          AbstractValue src,
                          AbstractValue dest,
                          const llvm::Instruction* loc);
    
    /**
     * Sanitize a tainted value: mark it as sanitized
     */
    static void sanitize(AbductiveDomain& astate,
                         AbstractValue v,
                         TaintKind sanitizer_kind,
                         const llvm::Instruction* sanitizer_loc);
    
    /**
     * Check if a value is sanitized
     */
    static bool isSanitized(const AbductiveDomain& astate, AbstractValue v);
    
    /**
     * Propagate taint through memory operations (load/store)
     */
    static void propagateThroughLoad(AbductiveDomain& astate,
                                     AbstractValue src_addr,
                                     AbstractValue dest_val,
                                     const llvm::Instruction* loc);
    
    static void propagateThroughStore(AbductiveDomain& astate,
                                      AbstractValue src_val,
                                      AbstractValue dest_addr,
                                      const llvm::Instruction* loc);
    
    /**
     * Propagate taint through function calls
     */
    static void propagateThroughCall(AbductiveDomain& astate,
                                      const llvm::CallInst* call,
                                      const std::vector<AbstractValue>& args,
                                      AbstractValue ret_val);
    
    /**
     * Get next timestamp
     */
    static unsigned getNextTimestamp() { return global_timestamp_++; }
    
    /**
     * Reset timestamp counter
     */
    static void resetTimestamp() { global_timestamp_ = 1; }
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSETAINT_H
