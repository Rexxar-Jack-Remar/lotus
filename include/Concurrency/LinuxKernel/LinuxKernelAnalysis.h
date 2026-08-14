/**
 * @file LinuxKernelAnalysis.h
 * @brief Linux Kernel Concurrency Analysis
 *
 * This file is the main entry point for Linux Kernel concurrency analysis.
 * It coordinates all kernel-specific analyses including lock analysis,
 * RCU analysis, wait queue analysis, and more.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_ANALYSIS_H
#define LINUX_KERNEL_ANALYSIS_H

#include "Concurrency/LinuxKernel/LinuxKernelConfig.h"
#include "Concurrency/LinuxKernel/LinuxKernelExecutionGraph.h"
#include "Concurrency/LinuxKernel/LinuxKernelLifetimeAnalysis.h"
#include "Concurrency/LinuxKernel/LinuxKernelLockAnalysis.h"
#include "Concurrency/LinuxKernel/LinuxKernelMemoryModel.h"
#include "Concurrency/LinuxKernel/LinuxKernelOperation.h"
#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"
#include "Concurrency/LinuxKernel/LinuxKernelRCUAnalysis.h"
#include "Concurrency/LinuxKernel/LinuxKernelWaitAnalysis.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
class HappensBeforeAnalysis;
} // namespace lotus

namespace mhp {
class IMHPAnalysis;
} // namespace mhp

namespace kernel {

struct LinuxKernelAnalysisServices {
  lotus::AliasAnalysisWrapper *alias_analysis = nullptr;
  const mhp::IMHPAnalysis *mhp_analysis = nullptr;
  const lotus::HappensBeforeAnalysis *happens_before = nullptr;
};

class LinuxKernelAnalysis {
public:
  enum class FindingConfidence { PROVED, POSSIBLE, DEFERRED };

  struct DiagnosticFinding {
    std::string stable_id;
    std::string category;
    std::string severity;
    FindingConfidence confidence = FindingConfidence::POSSIBLE;
    const llvm::Instruction *primary = nullptr;
    const llvm::Instruction *secondary = nullptr;
    std::vector<const llvm::Instruction *> witness;
    std::vector<std::string> assumptions;
    std::vector<std::string> unresolved;
    std::string rationale;
  };

  explicit LinuxKernelAnalysis(
      llvm::Module &M, bool preempt_rt = false,
      LinuxKernelAnalysisServices services = LinuxKernelAnalysisServices())
      : LinuxKernelAnalysis(M, LinuxKernelConfig::withPreemptRT(preempt_rt),
                            services) {}

  LinuxKernelAnalysis(
      llvm::Module &M, LinuxKernelConfig config,
      LinuxKernelAnalysisServices services = LinuxKernelAnalysisServices())
      : module_(M), config_(std::move(config)), process_model_(M, config_),
        execution_graph_(process_model_),
        lock_analysis_(process_model_, &execution_graph_),
        memory_model_(process_model_, execution_graph_, lock_analysis_),
        lifetime_analysis_(process_model_, execution_graph_),
        rcu_analysis_(process_model_),
        wait_analysis_(process_model_, &execution_graph_), services_(services) {
    process_model_.setAliasAnalysis(services_.alias_analysis);
    execution_graph_.setMHPAnalysis(services_.mhp_analysis);
    execution_graph_.setHappensBeforeAnalysis(services_.happens_before);
  }

  void runAnalysis();

  void printResults(llvm::raw_ostream &OS) const;
  void printSARIF(llvm::raw_ostream &OS) const;

  struct AnalysisResults {
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        lock_deadlocks;
    std::vector<const llvm::Instruction *> double_locks;
    std::vector<const llvm::Instruction *> unlock_without_lock;
    std::vector<const llvm::Instruction *> lock_without_unlock;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        lock_order_inversions;
    std::vector<LinuxKernelLockAnalysis::LockDependencyCycle>
        lock_dependency_cycles;
    std::vector<const llvm::Instruction *> sleep_in_atomic;
    std::vector<const llvm::Instruction *> mix_raw_and_cooked;
    std::vector<const llvm::Instruction *> irq_save_restore_issues;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        data_races;
    std::vector<const llvm::Instruction *> unresolved_calls;

    std::vector<const llvm::Instruction *> rcu_without_grace_period;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        rcu_conflicts;
    std::vector<const llvm::Instruction *> rcu_double_free;
    std::vector<const llvm::Instruction *> deref_after_free;
    std::vector<const llvm::Instruction *> rcu_unsafe_reclamation;

    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        missing_wake_ups;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        spurious_wake_ups;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        lost_wakeups;
    std::vector<const llvm::Instruction *> missing_completion;
    std::vector<const llvm::Instruction *> double_completion;
    std::vector<const llvm::Instruction *> timer_not_deleted;
    std::vector<const llvm::Instruction *> timer_use_after_delete;

    std::vector<const llvm::Instruction *> use_after_free;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        async_lifetime_hazards;
    std::vector<const llvm::Instruction *> timer_issues;
    std::vector<DiagnosticFinding> diagnostics;
  };

  const AnalysisResults &getResults() const { return results_; }

  size_t getOperationCount(OperationKind kind) const;
  size_t getLockCount() const;
  size_t getRCUSectionCount() const;
  size_t getWaitQueueCount() const;

  const LinuxKernelProcessModel &getProcessModel() const {
    return process_model_;
  }
  const LinuxKernelConfig &getConfig() const { return config_; }
  const LinuxKernelLockAnalysis &getLockAnalysis() const {
    return lock_analysis_;
  }
  const LinuxKernelExecutionGraph &getExecutionGraph() const {
    return execution_graph_;
  }
  const LinuxKernelMemoryModel &getMemoryModel() const { return memory_model_; }
  const LinuxKernelLifetimeAnalysis &getLifetimeAnalysis() const {
    return lifetime_analysis_;
  }
  const LinuxKernelRCUAnalysis &getRCUAnalysis() const { return rcu_analysis_; }
  const LinuxKernelWaitAnalysis &getWaitAnalysis() const {
    return wait_analysis_;
  }

private:
  llvm::Module &module_;
  LinuxKernelConfig config_;

  LinuxKernelProcessModel process_model_;
  LinuxKernelExecutionGraph execution_graph_;
  LinuxKernelLockAnalysis lock_analysis_;
  LinuxKernelMemoryModel memory_model_;
  LinuxKernelLifetimeAnalysis lifetime_analysis_;
  LinuxKernelRCUAnalysis rcu_analysis_;
  LinuxKernelWaitAnalysis wait_analysis_;
  LinuxKernelAnalysisServices services_;

  AnalysisResults results_;

  void buildDiagnostics();
};

} // namespace kernel

#endif // LINUX_KERNEL_ANALYSIS_H
