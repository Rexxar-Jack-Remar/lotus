/**
 * @file OpenMPTaskGraph.h
 * @brief OpenMP Task Dependency Graph
 *
 * This file provides infrastructure for tracking OpenMP task dependencies
 * via the depend clause.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#pragma once

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OpenMP {

/**
 * @brief Dependency type for OpenMP tasks
 */
enum class DependType {
  IN,      ///< Input dependency (read)
  OUT,     ///< Output dependency (write)
  INOUT,   ///< Input/output dependency (read-write)
  MUTEXINOUTSET ///< Mutex inoutset (OpenMP 5.0+)
};

enum class DependencySourceKind {
  DirectAddress,
  RegionSummary,
  DepObj,
  Iterator,
  Unknown
};

enum class DependencyProof {
  Definite,
  Possible,
  Unknown
};

/**
 * @brief Represents a dependency on a memory location
 */
struct Dependency {
  DependType type;
  const llvm::Value *address;  ///< Address expression
  size_t size;                 ///< Size in bytes (0 if unknown)
  const llvm::Value *canonical_base = nullptr; ///< Best-effort symbolic region key
  int64_t offset = 0;          ///< Constant offset from canonical base when known
  bool has_precise_offset = false;
  DependencySourceKind source_kind = DependencySourceKind::Unknown;
  DependencyProof proof = DependencyProof::Unknown;
};

/**
 * @brief Represents an OpenMP task
 */
struct Task {
  const llvm::Instruction *task_create;  ///< __kmpc_omp_task call
  const llvm::Function *task_function;   ///< Task body function
  const llvm::Function *parent_context = nullptr; ///< Scheduling context
  const llvm::Instruction *generating_context = nullptr; ///< Active lexical region
  size_t scheduling_context_id = 0;      ///< Stable scheduling context across helper calls
  size_t taskgroup_id = 0;               ///< Innermost taskgroup if known
  size_t phase_id = 0;                   ///< taskwait/taskgroup phase within parent context
  size_t sibling_group = 0;              ///< Sibling-equivalence class for depend matching
  size_t sequence_index = 0;             ///< Instruction order within parent context
  size_t region_id = 0;                  ///< Lexical OpenMP region active at creation
  std::vector<Dependency> dependencies;  ///< Task dependencies
  std::set<Task *> predecessors;         ///< Tasks that must complete before this
  std::set<Task *> successors;           ///< Tasks that depend on this
  std::set<Task *> exclusions;           ///< Mutual exclusion without happens-before
};

/**
 * @class OpenMPTaskGraph
 * @brief Builds and analyzes OpenMP task dependency graph
 *
 * Analyzes OpenMP task constructs with depend clauses to build a
 * task dependency graph for precise happens-before analysis.
 */
class OpenMPTaskGraph {
public:
  enum class TaskRelation {
    HappensBefore,
    Excluded,
    Parallel,
    Unknown
  };

  struct WaitBoundaryInfo {
    enum class Kind {
      Taskwait,
      TaskwaitDeps,
      TaskgroupEnd,
      SingleEnd,
      SectionsEnd,
      ForFini,
      DispatchFini,
      Reduce,
      Unknown
    };

    const llvm::Instruction *inst = nullptr;
    size_t scheduling_context_id = 0;
    size_t sequence_index = 0;
    size_t phase_id = 0;
    size_t taskgroup_id = 0;
    size_t region_id = 0;
    bool is_taskgroup_end = false;
    bool is_partial_wait = false;
    Kind kind = Kind::Unknown;
  };

  explicit OpenMPTaskGraph(llvm::Module &module);
  
  /**
   * @brief Analyze the module to build task dependency graph
   */
  void analyze();
  
  /**
   * @brief Get all tasks in the program
   */
  const std::vector<std::unique_ptr<Task>> &getAllTasks() const {
    return m_tasks;
  }

  const Task *getTaskForCreate(const llvm::Instruction *inst) const;

  const std::vector<WaitBoundaryInfo> &getWaitBoundaries() const {
    return m_wait_boundary_infos;
  }

  size_t getDeferredWaitDepsCount() const { return m_deferred_wait_deps_count; }
  size_t getDeferredImpreciseConflictCount() const {
    return m_deferred_imprecise_conflict_count;
  }
  const std::unordered_map<std::string, size_t> &getDeferredReasonCounts() const {
    return m_deferred_reason_counts;
  }
  
  /**
   * @brief Check if two tasks have a happens-before relationship
   */
  bool happensBefore(const Task *t1, const Task *t2) const;

  /**
   * @brief Classify the relation between two tasks.
   *
   * `Unknown` means the analysis found evidence that the tasks are not
   * provably independent, but could not justify a definite happens-before
   * edge either.
   */
  TaskRelation classifyTaskRelation(const Task *t1, const Task *t2) const;
  
  /**
   * @brief Check if two tasks may execute in parallel
   */
  bool mayBeParallel(const Task *t1, const Task *t2) const;

private:
  struct WaitBoundary {
    const llvm::Instruction *inst = nullptr;
    size_t scheduling_context_id = 0;
    size_t sequence_index = 0;
    size_t sibling_group = 0;
    size_t taskgroup_id = 0;
    bool is_taskgroup_end = false;
  };

  struct TraversalState {
    struct RegionFrame {
      size_t id = 0;
      WaitBoundaryInfo::Kind kind = WaitBoundaryInfo::Kind::Unknown;
    };

    size_t scheduling_context_id = 0;
    size_t next_taskgroup_id = 1;
    size_t next_phase_token = 1;
    size_t next_region_id = 1;
    size_t sequence_index = 0;
    const llvm::Instruction *anchor_inst = nullptr;
    std::vector<size_t> taskgroup_stack;
    std::vector<size_t> phase_stack;
    std::vector<RegionFrame> region_stack;
  };

  llvm::Module &m_module;
  std::vector<std::unique_ptr<Task>> m_tasks;
  std::map<const llvm::Instruction *, Task *> m_inst_to_task;
  std::unordered_map<size_t, std::vector<WaitBoundary>> m_wait_boundaries;
  std::vector<WaitBoundaryInfo> m_wait_boundary_infos;
  std::set<std::pair<const Task *, const Task *>> m_unknown_relations;
  size_t m_next_scheduling_context_id = 1;
  size_t m_deferred_wait_deps_count = 0;
  mutable size_t m_deferred_imprecise_conflict_count = 0;
  std::unordered_map<std::string, size_t> m_deferred_reason_counts;
  
  /**
   * @brief Identify all OpenMP task creation sites
   */
  void identifyTasks();
  void scanSchedulingContext(const llvm::Function *func, TraversalState &state,
                            std::set<const llvm::Function *> &call_stack);
  
  /**
   * @brief Extract dependencies from task creation call
   */
  std::vector<Dependency> extractDependencies(const llvm::CallBase *task_call);
  const llvm::Function *extractTaskFunction(const llvm::CallBase *task_call);
  
  /**
   * @brief Build dependency edges between tasks
   */
  void buildDependencyEdges();
  
  /**
   * @brief Check if two dependencies conflict
   */
  bool dependenciesConflict(const Dependency &d1, const Dependency &d2) const;

  bool isMutexLikeExclusion(const Dependency &d1, const Dependency &d2) const;
};

} // namespace OpenMP
