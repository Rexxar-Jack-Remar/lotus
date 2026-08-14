/**
 * @file LinuxKernelLockAnalysis.h
 * @brief Linux Kernel Lock Analysis
 *
 * This file provides analysis for kernel lock primitives including
 * spinlocks, mutexes, semaphores, and read-write locks.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_LOCK_ANALYSIS_H
#define LINUX_KERNEL_LOCK_ANALYSIS_H

#include "Concurrency/LinuxKernel/LinuxKernelOperation.h"
#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kernel {

class LinuxKernelProcessModel;
class LinuxKernelExecutionGraph;

class LinuxKernelLockAnalysis {
public:
  explicit LinuxKernelLockAnalysis(
      const LinuxKernelProcessModel &model,
      const LinuxKernelExecutionGraph *execution_graph = nullptr)
      : process_model_(model), execution_graph_(execution_graph) {}

  void analyzeLocks();

  struct LockRegion {
    const llvm::Instruction *acquire_inst;
    const llvm::Instruction *release_inst;
    LockID lock;
    LockKind kind;
    bool is_critical;
  };

  enum class DependencyKind { ER, EN, SR, SN };

  struct LockDependencyCycle {
    std::vector<LockClassID> classes;
    std::vector<const llvm::Instruction *> evidence;
  };

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findPotentialDeadlocks() const;

  std::vector<const llvm::Instruction *> findDoubleLocks() const;

  std::vector<const llvm::Instruction *> findUnlockWithoutLock() const;

  std::vector<const llvm::Instruction *> findLockWithoutUnlock() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findLockOrderInversions() const;

  std::vector<const llvm::Instruction *> findMixRawAndCookedLocks() const;

  std::vector<const llvm::Instruction *> findSleepInSpinlock() const;

  std::vector<const llvm::Instruction *> findIrqSaveRestoreIssues() const;

  std::vector<LockRegion> getLockRegions() const { return lock_regions_; }
  const std::vector<LockDependencyCycle> &getDependencyCycles() const {
    return lock_dependency_cycles_;
  }

  const std::unordered_map<std::string, size_t> &getLockDiagnostics() const {
    return lock_diagnostics_;
  }

private:
  struct LockFlowState {
    std::set<LockID> may_held;
    std::set<LockID> must_held;
    std::set<LockID> may_exclusive;
    std::set<LockID> must_exclusive;
    std::set<LockID> may_nonrecursive_reader;
    std::set<LockID> must_nonrecursive_reader;

    bool operator==(const LockFlowState &other) const {
      return may_held == other.may_held && must_held == other.must_held &&
             may_exclusive == other.may_exclusive &&
             must_exclusive == other.must_exclusive &&
             may_nonrecursive_reader == other.may_nonrecursive_reader &&
             must_nonrecursive_reader == other.must_nonrecursive_reader;
    }
    bool operator!=(const LockFlowState &other) const {
      return !(*this == other);
    }
  };

  const LinuxKernelProcessModel &process_model_;
  const LinuxKernelExecutionGraph *execution_graph_ = nullptr;
  std::vector<LockRegion> lock_regions_;
  mutable std::unordered_map<std::string, size_t> lock_diagnostics_;
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      lock_order_inversions_;
  std::vector<const llvm::Instruction *> double_locks_;
  std::vector<const llvm::Instruction *> unlock_without_lock_;
  std::vector<const llvm::Instruction *> lock_without_unlock_;
  std::vector<const llvm::Instruction *> sleep_in_spinlock_;
  std::vector<LockDependencyCycle> lock_dependency_cycles_;

  bool isLockAcquire(OperationKind kind) const;
  bool isLockRelease(OperationKind kind) const;

  bool formsDeadlock(const LockRegion &r1, const LockRegion &r2) const;
  std::vector<LockID> getLockChain(const llvm::Instruction *inst) const;
};

} // namespace kernel

#endif // LINUX_KERNEL_LOCK_ANALYSIS_H
