#ifndef CHECKER_PULSE_PULSEDISJUNCTIVEDOMAIN_H
#define CHECKER_PULSE_PULSEDISJUNCTIVEDOMAIN_H

#include "Checker/Pulse/PulseDomain.h"
#include <llvm/IR/BasicBlock.h>
#include <map>
#include <set>
#include <vector>

namespace llvm {
class BasicBlock;
} // namespace llvm

namespace pulse {

/**
 * DisjunctiveDomain: manages multiple disjunctive states
 * Implements proper disjunctive abstract domain with widening
 */
class DisjunctiveDomain {
public:
    // A disjunct is a pair of (execution state, path context)
    // For now, path context is just the basic block
    struct Disjunct {
        ExecutionDomain state;
        const llvm::BasicBlock* path_context;
        
        Disjunct() : path_context(nullptr) {}
        
        Disjunct(ExecutionDomain s, const llvm::BasicBlock* ctx)
            : state(std::move(s)), path_context(ctx) {}
    };
    
private:
    std::vector<Disjunct> disjuncts_;
    static constexpr size_t kMaxDisjuncts = 10;
    static constexpr unsigned kWidenThreshold = 3;  // Widen after 3 iterations
    static constexpr size_t kWidenKeepDisjuncts = 4;
    
    // Track iterations per block for widening
    std::map<const llvm::BasicBlock*, unsigned> block_iterations_;
    
public:
    DisjunctiveDomain() = default;
    
    /**
     * Add a disjunct
     */
    void add(ExecutionDomain state, const llvm::BasicBlock* path_context);
    
    /**
     * Get all disjuncts
     */
    const std::vector<Disjunct>& getDisjuncts() const { return disjuncts_; }
    std::vector<Disjunct>& getDisjuncts() { return disjuncts_; }
    
    /**
     * Join disjuncts at a block entry
     * Returns the joined state (or first state if only one)
     */
    ExecutionDomain joinAtBlock(const llvm::BasicBlock* BB);
    
    /**
     * Widen: apply widening operator if needed
     */
    void widen(const llvm::BasicBlock* BB);
    
    /**
     * Check if we should widen at this block
     */
    bool shouldWiden(const llvm::BasicBlock* BB) const;
    
    /**
     * Clear disjuncts
     */
    void clear() { disjuncts_.clear(); }
    
    /**
     * Get number of disjuncts
     */
    size_t size() const { return disjuncts_.size(); }
    
    /**
     * Check if empty
     */
    bool empty() const { return disjuncts_.empty(); }
    
    /**
     * Limit disjuncts to max
     */
    void limitDisjuncts();
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEDISJUNCTIVEDOMAIN_H
