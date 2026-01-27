
#include "Checker/Pulse/PulseDisjunctiveDomain.h"
#include <algorithm>

namespace pulse {

static void reduceDisjuncts(std::vector<DisjunctiveDomain::Disjunct>& disjuncts, size_t max) {
    if (disjuncts.size() <= max) {
        return;
    }

    std::vector<DisjunctiveDomain::Disjunct> selected;
    selected.reserve(max);
    std::set<const llvm::BasicBlock*> seen_ctx;

    for (auto& d : disjuncts) {
        if (selected.size() >= max) {
            break;
        }
        if (!d.path_context) {
            continue;
        }
        if (seen_ctx.insert(d.path_context).second) {
            selected.push_back(std::move(d));
        }
    }

    for (auto& d : disjuncts) {
        if (selected.size() >= max) {
            break;
        }
        selected.push_back(std::move(d));
    }

    disjuncts.swap(selected);
}

void DisjunctiveDomain::add(ExecutionDomain state, const llvm::BasicBlock* path_context) {
    disjuncts_.emplace_back(std::move(state), path_context);
    limitDisjuncts();
}

void DisjunctiveDomain::limitDisjuncts() {
    reduceDisjuncts(disjuncts_, kMaxDisjuncts);
}

ExecutionDomain DisjunctiveDomain::joinAtBlock(const llvm::BasicBlock* BB) {
    (void)BB;
    if (disjuncts_.empty()) {
        return ExecutionDomain();
    }
    
    if (disjuncts_.size() == 1) {
        return disjuncts_[0].state.clone();
    }
    
    // Join multiple disjuncts
    std::vector<const AbductiveDomain*> astates;
    for (const auto& disj : disjuncts_) {
        if (!disj.state.isStopped()) {
            const AbductiveDomain* astate = disj.state.getAstate();
            if (astate) {
                astates.push_back(astate);
            }
        }
    }
    
    if (astates.empty()) {
        ExecutionDomain stopped;
        stopped.setState(ExecutionState::Stopped);
        return stopped;
    }
    
    if (astates.size() == 1) {
        ExecutionDomain result;
        auto astate_ptr = std::make_unique<AbductiveDomain>(astates[0]->clone());
        // Note: ExecutionDomain doesn't expose setAstate, so we need to construct it
        return ExecutionDomain(std::move(astate_ptr));
    }
    
    // Merge multiple states
    AbductiveDomain merged = astates[0]->clone();
    PulseFormula merged_formula = astates[0]->getPathFormula().clone();
    
    for (size_t i = 1; i < astates.size(); ++i) {
        auto merge_result = AbductiveDomain::merge(merged, *astates[i]);
        if (!merge_result) {
            // Contradiction - use first state
            merged = astates[0]->clone();
            merged_formula = astates[0]->getPathFormula().clone();
            break;
        }
        merged = merge_result->clone();
        
        PulseFormula new_formula = PulseFormula::merge(merged_formula, 
                                                       astates[i]->getPathFormula());
        if (new_formula.isConsistent()) {
            merged_formula = std::move(new_formula);
        } else {
            // Formula contradiction - use first state only
            merged = astates[0]->clone();
            merged_formula = astates[0]->getPathFormula().clone();
            break;
        }
    }
    
    auto merged_ptr = std::make_unique<AbductiveDomain>(std::move(merged));
    merged_ptr->setPathFormula(std::make_unique<PulseFormula>(std::move(merged_formula)));
    ExecutionDomain result(std::move(merged_ptr));
    return result;
}

bool DisjunctiveDomain::shouldWiden(const llvm::BasicBlock* BB) const {
    auto it = block_iterations_.find(BB);
    if (it == block_iterations_.end()) {
        return false;
    }
    return it->second >= kWidenThreshold;
}

void DisjunctiveDomain::widen(const llvm::BasicBlock* BB) {
    auto it = block_iterations_.find(BB);
    if (it == block_iterations_.end()) {
        block_iterations_[BB] = 1;
    } else {
        it->second++;
    }
    
    // Apply widening if threshold reached
    if (block_iterations_[BB] >= kWidenThreshold) {
        reduceDisjuncts(disjuncts_, kWidenKeepDisjuncts);
    }
}

} // namespace pulse
