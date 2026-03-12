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
#include <set>
#include <unordered_map>
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

/**
 * @brief Represents a dependency on a memory location
 */
struct Dependency {
  DependType type;
  const llvm::Value *address;  ///< Address expression
  size_t size;                 ///< Size in bytes (0 if unknown)
};

/**
 * @brief Represents an OpenMP task
 */
struct Task {
  const llvm::Instruction *task_create;  ///< __kmpc_omp_task call
  const llvm::Function *task_function;   ///< Task body function
  const llvm::Function *parent_context = nullptr; ///< Scheduling context
  size_t taskgroup_id = 0;               ///< Innermost taskgroup if known
  size_t sequence_index = 0;             ///< Instruction order within parent context
  std::vector<Dependency> dependencies;  ///< Task dependencies
  std::set<Task *> predecessors;         ///< Tasks that must complete before this
  std::set<Task *> successors;           ///< Tasks that depend on this
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
  
  /**
   * @brief Check if two tasks have a happens-before relationship
   */
  bool happensBefore(const Task *t1, const Task *t2) const;
  
  /**
   * @brief Check if two tasks may execute in parallel
   */
  bool mayBeParallel(const Task *t1, const Task *t2) const;

private:
  llvm::Module &m_module;
  std::vector<std::unique_ptr<Task>> m_tasks;
  std::map<const llvm::Instruction *, Task *> m_inst_to_task;
  std::unordered_map<const llvm::Function *, std::vector<size_t>> m_wait_boundaries;
  
  /**
   * @brief Identify all OpenMP task creation sites
   */
  void identifyTasks();
  
  /**
   * @brief Extract dependencies from task creation call
   */
  std::vector<Dependency> extractDependencies(const llvm::CallBase *task_call);
  
  /**
   * @brief Build dependency edges between tasks
   */
  void buildDependencyEdges();
  
  /**
   * @brief Check if two dependencies conflict
   */
  bool dependenciesConflict(const Dependency &d1, const Dependency &d2) const;
};

} // namespace OpenMP
