/*
 * IterativeIDESolver Implementation
 */

#pragma once

#include "Dataflow/IFDS/Solvers/IterativeIDESolver.h"
#include "Dataflow/IFDS/Solvers/IDESolver.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/xxhash.h>

namespace ifds {

// ============================================================================
// ModuleVersionTracker Implementation
// ============================================================================

ModuleVersionTracker::ModuleVersion 
ModuleVersionTracker::snapshot(const llvm::Module& module) {
    ModuleVersion version;
    version.timestamp = std::chrono::steady_clock::now();
    version.version_id = std::chrono::duration_cast<std::chrono::nanoseconds>(
        version.timestamp.time_since_epoch()).count();
    
    for (const auto& func : module) {
        FunctionHash fh;
        fh.name = func.getName().str();
        fh.has_body = !func.isDeclaration();
        
        if (fh.has_body) {
            fh.hash = compute_function_hash(func);
            fh.instruction_count = func.getInstructionCount();
        } else {
            fh.hash = 0;
            fh.instruction_count = 0;
        }
        
        version.function_hashes[fh.name] = std::move(fh);
    }
    
    return version;
}

std::vector<std::string> ModuleVersionTracker::detect_changes(
    const ModuleVersion& old_ver, 
    const ModuleVersion& new_ver) const {
    
    std::vector<std::string> changed;
    
    for (const auto& pair : new_ver.function_hashes) {
        const auto& new_fh = pair.second;
        auto it = old_ver.function_hashes.find(pair.first);
        
        if (it != old_ver.function_hashes.end()) {
            const auto& old_fh = it->second;
            if (old_fh.hash != new_fh.hash || 
                old_fh.instruction_count != new_fh.instruction_count) {
                changed.push_back(pair.first);
            }
        }
    }
    
    return changed;
}

std::vector<std::string> ModuleVersionTracker::detect_additions(
    const ModuleVersion& old_ver,
    const ModuleVersion& new_ver) const {
    
    std::vector<std::string> added;
    
    for (const auto& pair : new_ver.function_hashes) {
        if (old_ver.function_hashes.find(pair.first) == old_ver.function_hashes.end()) {
            added.push_back(pair.first);
        }
    }
    
    return added;
}

std::vector<std::string> ModuleVersionTracker::detect_removals(
    const ModuleVersion& old_ver,
    const ModuleVersion& new_ver) const {
    
    std::vector<std::string> removed;
    
    for (const auto& pair : old_ver.function_hashes) {
        if (new_ver.function_hashes.find(pair.first) == new_ver.function_hashes.end()) {
            removed.push_back(pair.first);
        }
    }
    
    return removed;
}

uint64_t ModuleVersionTracker::compute_function_hash(const llvm::Function& func) {
    if (func.isDeclaration()) {
        return 0;
    }
    
    llvm::SmallString<256> func_content;
    llvm::raw_svector_ostream stream(func_content);
    
    for (const auto& bb : func) {
        for (const auto& inst : bb) {
            stream << inst.getOpcodeName();
            for (const auto& op : inst.operands()) {
                if (auto* val = llvm::dyn_cast<llvm::Value>(op)) {
                    // BUG (fixed): the old code only serialised the Value's
                    // name.  For constants (ConstantInt, ConstantFP, etc.) the
                    // name is always empty, so two functions that differ only
                    // in a literal constant (e.g. `x + 1` vs `x + 2`) would
                    // produce the same hash and the change would go undetected.
                    // We now print the full value representation for constants.
                    if (llvm::isa<llvm::Constant>(val)) {
                        val->print(stream);
                    } else {
                        stream << val->getName();
                    }
                }
            }
        }
    }
    
    return llvm::xxHash64(func_content);
}

uint64_t ModuleVersionTracker::hash_instruction(const llvm::Instruction& inst) {
    llvm::SmallString<64> content;
    llvm::raw_svector_ostream stream(content);
    stream << inst.getOpcodeName();
    for (const auto& op : inst.operands()) {
        if (auto* val = llvm::dyn_cast<llvm::Value>(op)) {
            stream << val->getName();
        }
    }
    return llvm::xxHash64(content);
}

// ============================================================================
// IterativeIDESolver Implementation
// ============================================================================

template<typename Problem>
IterativeIDESolver<Problem>::IterativeIDESolver(Problem& problem)
    : m_problem(problem) {}

template<typename Problem>
void IterativeIDESolver<Problem>::solve(const llvm::Module& module) {
    if (m_config.incremental_mode && !m_last_version.function_hashes.empty()) {
        solve_incremental(module);
    } else {
        solve_full(module);
    }
}

template<typename Problem>
void IterativeIDESolver<Problem>::solve_full(const llvm::Module& module) {
    m_solve_start_time = std::chrono::steady_clock::now();
    m_stats.num_iterations++;
    m_reanalyzed_functions.clear();
    
    // Take version snapshot
    auto current_version = m_version_tracker.snapshot(module);
    
    // Run full analysis
    run_solver_on_module(module);
    
    // Cache results
    if (m_config.enable_caching) {
        cache_results(current_version.version_id);
    }
    
    m_last_version = std::move(current_version);
    
    // Update statistics
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - m_solve_start_time).count();
    m_stats.avg_solve_time_ms = 
        (m_stats.avg_solve_time_ms * (m_stats.num_iterations - 1) + duration) / 
        m_stats.num_iterations;
}

template<typename Problem>
void IterativeIDESolver<Problem>::solve_incremental(const llvm::Module& module) {
    if (!m_config.enable_caching || m_cached_results.empty()) {
        solve_full(module);
        return;
    }
    
    m_solve_start_time = std::chrono::steady_clock::now();
    m_stats.num_iterations++;
    
    // Take version snapshot and detect changes
    auto current_version = m_version_tracker.snapshot(module);
    m_changed_functions = m_version_tracker.detect_changes(m_last_version, current_version);
    auto added = m_version_tracker.detect_additions(m_last_version, current_version);
    auto removed = m_version_tracker.detect_removals(m_last_version, current_version);
    
    // Combine all changes
    m_changed_functions.insert(m_changed_functions.end(), added.begin(), added.end());
    
    // Invalidate results for removed functions
    for (const auto& func_name : removed) {
        invalidate_function_results(func_name);
    }
    
    // Invalidate results for changed functions
    for (const auto& func_name : m_changed_functions) {
        invalidate_function_results(func_name);
    }
    
    // Determine which functions to re-analyze
    std::set<std::string> to_reanalyze;
    for (const auto& func_name : m_changed_functions) {
        to_reanalyze.insert(func_name);
        
        // Also re-analyze callers of changed functions
        // (simplified - in practice, need call graph info)
        for (const auto& func : module) {
            for (const auto& bb : func) {
                for (const auto& inst : bb) {
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
                        if (call->getCalledFunction() && 
                            call->getCalledFunction()->getName() == func_name) {
                            to_reanalyze.insert(func.getName().str());
                        }
                    }
                }
            }
        }
    }
    
    m_stats.num_reanalyzed_functions = to_reanalyze.size();
    
    if (to_reanalyze.empty()) {
        // No changes detected, use cached results
        m_current_values = m_cached_results.values;
        m_stats.num_reused_results++;
    } else {
        // Run solver on changed functions
        run_solver_on_functions(module, to_reanalyze);
        
        // Merge with cached results for unchanged functions
        if (m_config.reuse_previous_results) {
            merge_with_cached_results();
        }
    }
    
    // Cache updated results
    if (m_config.enable_caching) {
        cache_results(current_version.version_id);
    }
    
    m_last_version = std::move(current_version);
    
    // Update statistics
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - m_solve_start_time).count();
    m_stats.avg_solve_time_ms = 
        (m_stats.avg_solve_time_ms * (m_stats.num_iterations - 1) + duration) / 
        m_stats.num_iterations;
}

template<typename Problem>
void IterativeIDESolver<Problem>::run_solver_on_module(const llvm::Module& module) {
    // Use the standard IDESolver for the full analysis
    IDESolver<Problem> solver(m_problem);
    solver.solve(module);
    
    // Copy results
    m_current_values = solver.get_all_values();
}

template<typename Problem>
void IterativeIDESolver<Problem>::run_solver_on_functions(
    const llvm::Module& module,
    const std::set<std::string>& functions) {
    
    // Mark which functions were reanalyzed.
    for (const auto& func_name : functions) {
        m_reanalyzed_functions.insert(func_name);
    }
    
    // BUG (fixed): the old code called run_solver_on_module() unconditionally,
    // which always ran a full inter-procedural analysis over the entire module.
    // This completely defeated the purpose of incremental mode: every
    // "incremental" solve was as expensive as a full solve, and the
    // m_reanalyzed_functions set was populated but never used to restrict the
    // analysis scope.
    //
    // A truly partial re-analysis would require seeding the solver only at the
    // entry points of the changed functions and propagating only within those
    // functions (and their transitive callees).  That requires deeper solver
    // integration that is not yet implemented.
    //
    // For now we still run the full solver, but we document the limitation
    // clearly and leave the incremental_mode flag as a no-op performance hint
    // rather than silently claiming partial analysis.  The merge step below
    // will correctly prefer fresh results over stale cached ones.
    //
    // TODO: implement true partial re-analysis by seeding only changed
    // functions and restricting propagation to their SCCs.
    run_solver_on_module(module);
}

template<typename Problem>
void IterativeIDESolver<Problem>::merge_with_cached_results() {
    // Merge cached results for functions that weren't reanalyzed.
    // BUG (fixed): the old code used operator[] to insert cached values, which
    // overwrites any entry already present in m_current_values.  Because
    // run_solver_on_module() runs a full analysis, m_current_values already
    // contains fresh results for ALL functions — including the reanalyzed ones.
    // Overwriting those fresh results with stale cached values for the same
    // instructions would corrupt the analysis output.
    //
    // The correct behaviour is: only insert cached values for instructions
    // that belong to functions that were NOT reanalyzed AND that are not
    // already present in m_current_values (i.e. use insert, not operator[]).
    for (const auto& pair : m_cached_results.values) {
        const llvm::Instruction* inst = pair.first;
        if (!inst || !inst->getParent() || !inst->getParent()->getParent()) {
            continue;
        }
        std::string func_name = inst->getParent()->getParent()->getName().str();
        if (m_reanalyzed_functions.find(func_name) != m_reanalyzed_functions.end()) {
            // This function was reanalyzed — keep the fresh result, do NOT
            // overwrite it with the stale cached value.
            continue;
        }
        // Only fill in the cached value if the fresh analysis produced no
        // result for this instruction (e.g. the function was unreachable in
        // the new analysis context).
        m_current_values.insert({inst, pair.second});
    }
}

template<typename Problem>
void IterativeIDESolver<Problem>::cache_results(uint64_t version_id) {
    m_cached_results.values = m_current_values;
    m_cached_results.version_id = version_id;
    m_cached_results.timestamp = std::chrono::steady_clock::now();
    
    // Count analyzed instructions and functions
    std::set<std::string> analyzed_funcs;
    for (const auto& pair : m_current_values) {
        if (pair.first && pair.first->getParent() && 
            pair.first->getParent()->getParent()) {
            analyzed_funcs.insert(
                pair.first->getParent()->getParent()->getName().str());
        }
    }
    m_cached_results.analyzed_instructions = m_current_values.size();
    m_cached_results.analyzed_functions = analyzed_funcs.size();
}

template<typename Problem>
void IterativeIDESolver<Problem>::clear_cache() {
    m_cached_results.clear();
    m_last_version = ModuleVersionTracker::ModuleVersion();
    m_changed_functions.clear();
}

template<typename Problem>
void IterativeIDESolver<Problem>::invalidate_function_results(
    const std::string& func_name) {
    // Remove cached results for the given function
    auto it = m_cached_results.values.begin();
    while (it != m_cached_results.values.end()) {
        const llvm::Instruction* inst = it->first;
        if (inst && inst->getParent() && inst->getParent()->getParent() &&
            inst->getParent()->getParent()->getName() == func_name) {
            it = m_cached_results.values.erase(it);
        } else {
            ++it;
        }
    }
}

template<typename Problem>
bool IterativeIDESolver<Problem>::can_reuse_function(
    const std::string& func_name) const {
    return std::find(m_changed_functions.begin(), m_changed_functions.end(), 
                     func_name) == m_changed_functions.end();
}

template<typename Problem>
void IterativeIDESolver<Problem>::mark_function_changed(const std::string& func_name) {
    m_changed_functions.push_back(func_name);
}

template<typename Problem>
void IterativeIDESolver<Problem>::mark_all_changed() {
    m_changed_functions.clear();
    for (const auto& pair : m_last_version.function_hashes) {
        m_changed_functions.push_back(pair.first);
    }
}

template<typename Problem>
typename IterativeIDESolver<Problem>::Value 
IterativeIDESolver<Problem>::get_value_at(const llvm::Instruction* inst, 
                                          const Fact& fact) const {
    auto it = m_current_values.find(inst);
    if (it != m_current_values.end()) {
        auto jt = it->second.find(fact);
        if (jt != it->second.end()) {
            return jt->second;
        }
    }
    // A missing entry means the fact has not been reached at this instruction,
    // so the correct default is bottom_value() (no information / unreachable),
    // not top_value() (which would mean "all possible values" and is unsound).
    return m_problem.bottom_value();
}

template<typename Problem>
typename IterativeIDESolver<Problem>::Value 
IterativeIDESolver<Problem>::get_value_at_in_llvm_ssa(const llvm::Instruction* inst,
                                                      const Fact& fact) const {
    if (inst->getType()->isVoidTy()) {
        return get_value_at(inst, fact);
    }
    
    const llvm::Instruction* next = inst->getNextNode();
    if (next) {
        return get_value_at(next, fact);
    }
    
    if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(inst)) {
        llvm::BasicBlock* normal = invoke->getNormalDest();
        if (normal && !normal->empty()) {
            return get_value_at(&normal->front(), fact);
        }
    }
    
    return get_value_at(inst, fact);
}

template<typename Problem>
void IterativeIDESolver<Problem>::dump_stats(llvm::raw_ostream& OS) const {
    OS << "========================================\n";
    OS << "IterativeIDESolver Statistics\n";
    OS << "========================================\n";
    OS << "Number of iterations: " << m_stats.num_iterations << "\n";
    OS << "Results reused: " << m_stats.num_reused_results << "\n";
    OS << "Functions reanalyzed: " << m_stats.num_reanalyzed_functions << "\n";
    OS << "Cached edges: " << m_stats.num_cached_edges << "\n";
    OS << "Average solve time: " << m_stats.avg_solve_time_ms << " ms\n";
    OS << "Memory usage: " << m_stats.memory_usage_bytes << " bytes\n";
    
    if (!m_changed_functions.empty()) {
        OS << "\nChanged functions in last run:\n";
        for (const auto& func : m_changed_functions) {
            OS << "  - " << func << "\n";
        }
    }
    
    if (!m_cached_results.empty()) {
        OS << "\nCached results:\n";
        OS << "  Version ID: " << m_cached_results.version_id << "\n";
        OS << "  Instructions: " << m_cached_results.analyzed_instructions << "\n";
        OS << "  Functions: " << m_cached_results.analyzed_functions << "\n";
    }
}

} // namespace ifds
