/*
 * IterativeIDESolver - Incremental IDE Analysis Solver
 *
 * This solver caches analysis results and supports incremental re-analysis
 * when the program changes. Based on the approach from:
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"
#include "Dataflow/IFDS/Core/IFDSIDESolverConfig.h"
#include "Dataflow/IFDS/Solvers/IDESolver.h"

#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Module.h>

namespace ifds {

// ============================================================================
// Versioned Analysis Result
// ============================================================================

template <typename Fact, typename Value> struct VersionedResult {
  using ResultMap = std::unordered_map<const llvm::Instruction *,
                                       std::unordered_map<Fact, Value>>;

  ResultMap values;
  uint64_t version_id;
  std::chrono::steady_clock::time_point timestamp;
  size_t analyzed_instructions;
  size_t analyzed_functions;

  VersionedResult()
      : version_id(0), analyzed_instructions(0), analyzed_functions(0) {}

  bool empty() const { return values.empty(); }
  void clear() { values.clear(); }
};

// ============================================================================
// Incremental Solver Configuration
// ============================================================================

struct IncrementalSolverConfig {
  // Enable caching of solver results between runs
  bool enable_caching = true;

  // Maximum number of cached versions to keep
  size_t max_cached_versions = 5;

  // Enable jump function garbage collection
  bool enable_gc = true;

  // Aggressive GC mode - removes more data but may require re-analysis
  bool aggressive_gc = false;

  // Compute and store edge function statistics
  bool enable_statistics = false;

  // Only re-analyze changed functions (vs. full re-analysis)
  bool incremental_mode = true;

  // Re-use previous results when possible
  bool reuse_previous_results = true;
};

// ============================================================================
// Module Version / Change Tracking
// ============================================================================

class ModuleVersionTracker {
public:
  struct FunctionHash {
    std::string name;
    uint64_t hash;
    size_t instruction_count;
    bool has_body;
  };

  struct ModuleVersion {
    uint64_t version_id;
    std::unordered_map<std::string, FunctionHash> function_hashes;
    std::chrono::steady_clock::time_point timestamp;
  };

  // Compute a version snapshot of the current module
  ModuleVersion snapshot(const llvm::Module &module);

  // Detect changed functions between two versions
  std::vector<std::string> detect_changes(const ModuleVersion &old_ver,
                                          const ModuleVersion &new_ver) const;

  // Detect added functions
  std::vector<std::string> detect_additions(const ModuleVersion &old_ver,
                                            const ModuleVersion &new_ver) const;

  // Detect removed functions
  std::vector<std::string> detect_removals(const ModuleVersion &old_ver,
                                           const ModuleVersion &new_ver) const;

private:
  uint64_t compute_function_hash(const llvm::Function &func);
  uint64_t hash_instruction(const llvm::Instruction &inst);
};

// ============================================================================
// Iterative IDE Solver
// ============================================================================

template <typename Problem> class IterativeIDESolver {
public:
  using Fact = typename Problem::FactType;
  using Value = typename Problem::ValueType;
  using EdgeFunction = typename Problem::EdgeFunction;
  using FactSet = typename Problem::FactSet;
  using ResultCache = VersionedResult<Fact, Value>;

  struct SolverStats {
    size_t num_iterations = 0;
    size_t num_reused_results = 0;
    size_t num_reanalyzed_functions = 0;
    size_t num_cached_edges = 0;
    double avg_solve_time_ms = 0.0;
    size_t memory_usage_bytes = 0;
  };

  explicit IterativeIDESolver(Problem &problem);

  // Configuration
  void set_config(const IncrementalSolverConfig &config) { m_config = config; }
  const IncrementalSolverConfig &get_config() const { return m_config; }

  // Main solve interface
  void solve(const llvm::Module &module);

  // Incremental solve - re-analyze only changed parts
  void solve_incremental(const llvm::Module &module);

  // Force full re-analysis
  void solve_full(const llvm::Module &module);

  // Query interface
  Value get_value_at(const llvm::Instruction *inst, const Fact &fact) const;
  Value get_value_at_in_llvm_ssa(const llvm::Instruction *inst,
                                 const Fact &fact) const;
  const ResultCache &get_cached_results() const { return m_cached_results; }

  // Result management
  void cache_results(uint64_t version_id);
  void clear_cache();
  bool has_cached_results() const { return !m_cached_results.empty(); }

  // Statistics
  SolverStats get_stats() const { return m_stats; }
  void dump_stats(llvm::raw_ostream &OS = llvm::outs()) const;

  // Change tracking
  std::vector<std::string> get_changed_functions() const {
    return m_changed_functions;
  }
  void mark_function_changed(const std::string &func_name);
  void mark_all_changed();

private:
  Problem &m_problem;
  IncrementalSolverConfig m_config;
  SolverStats m_stats;

  // Cached results from previous run
  ResultCache m_cached_results;

  // Current results
  std::unordered_map<const llvm::Instruction *, std::unordered_map<Fact, Value>>
      m_current_values;

  // Change tracking
  ModuleVersionTracker m_version_tracker;
  ModuleVersionTracker::ModuleVersion m_last_version;
  std::vector<std::string> m_changed_functions;
  std::set<std::string> m_reanalyzed_functions;

  // Statistics tracking
  std::chrono::steady_clock::time_point m_solve_start_time;

  // Internal methods
  void initialize_from_cache();
  void merge_with_cached_results();
  bool can_reuse_function(const std::string &func_name) const;
  void invalidate_function_results(const std::string &func_name);

  // Run the actual IDE solver on selected functions
  void run_solver_on_module(const llvm::Module &module);
  void run_solver_on_functions(const llvm::Module &module,
                               const std::set<std::string> &functions);
};

// ============================================================================
// Iterative IFDS Solver (wrapper around IDE with binary domain)
// ============================================================================

template <typename Problem> class IterativeIFDSSolver {
public:
  using Fact = typename Problem::FactType;
  using FactSet = typename Problem::FactSet;

  explicit IterativeIFDSSolver(Problem &problem);

  void set_config(const IncrementalSolverConfig &config);
  void solve(const llvm::Module &module);
  void solve_incremental(const llvm::Module &module);

  FactSet get_facts_at(const llvm::Instruction *inst) const;

private:
  Problem &m_problem;
  // Implementation wraps IterativeIDESolver with binary value domain
};

} // namespace ifds

// Template implementations
#include "Dataflow/IFDS/Solvers/IterativeIDESolver.tpp"
