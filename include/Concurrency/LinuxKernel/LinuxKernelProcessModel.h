/**
 * @file LinuxKernelProcessModel.h
 * @brief Linux Kernel Process/Thread Model
 *
 * This file provides the kernel process model that tracks kernel threads,
 * their operations, and relationships.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_PROCESS_MODEL_H
#define LINUX_KERNEL_PROCESS_MODEL_H

#include "Concurrency/LinuxKernel/LinuxKernelConfig.h"
#include "Concurrency/LinuxKernel/LinuxKernelOperation.h"
#include "Concurrency/LinuxKernel/LinuxKernelSemanticRegistry.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/Module.h>

namespace kernel {

class LinuxKernelLockAnalysis;
class LinuxKernelRCUAnalysis;
class LinuxKernelWaitAnalysis;

} // namespace kernel

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace kernel {

class LinuxKernelProcessModel {
public:
  struct ExecutionState {
    bool local_irq_disabled = false;
    bool bh_disabled = false;
    bool preempt_disabled = false;
  };

  explicit LinuxKernelProcessModel(llvm::Module &M, bool preempt_rt = false)
      : LinuxKernelProcessModel(M,
                                LinuxKernelConfig::withPreemptRT(preempt_rt)) {}
  LinuxKernelProcessModel(llvm::Module &M, LinuxKernelConfig config)
      : module_(M), config_(std::move(config)) {}

  void analyzeModule();

  const std::vector<KernelOperation> &getAllOperations() const {
    return all_operations_;
  }

  const std::unordered_map<OperationKind, size_t> &
  getOperationKindCounts() const {
    return operation_kind_counts_;
  }

  const llvm::Module &getModule() const { return module_; }
  bool isPreemptRT() const { return config_.isPreemptRT(); }
  const LinuxKernelConfig &getConfig() const { return config_; }
  const LinuxKernelSemanticRegistry &getSemanticRegistry() const {
    return semantic_registry_;
  }

  void setAliasAnalysis(lotus::AliasAnalysisWrapper *alias_analysis) {
    alias_analysis_ = alias_analysis;
  }

  std::vector<KernelOperation> getOperationsByKind(OperationKind kind) const;

  std::vector<KernelOperation> getOperationsByLock(LockID lock) const;

  std::vector<const KernelOperation *>
  getOperationsInFunction(const llvm::Function *function) const;

  const KernelOperation *
  getOperationForInstruction(const llvm::Instruction *inst) const;
  std::vector<const KernelOperation *>
  getOperationsForInstruction(const llvm::Instruction *inst) const;
  std::vector<const llvm::Function *>
  getPossibleCallees(const llvm::CallBase *call) const;

  bool isBeforeInFunction(const llvm::Instruction *lhs,
                          const llvm::Instruction *rhs) const;

  const llvm::Value *canonicalizeValue(const llvm::Value *value) const;
  LockClassID canonicalizeLockClass(const llvm::Value *value, LockKind kind,
                                    unsigned subclass = 0) const;
  bool mayAlias(const llvm::Value *lhs, const llvm::Value *rhs) const;
  bool mustAlias(const llvm::Value *lhs, const llvm::Value *rhs) const;
  bool getAliasSet(const llvm::Value *value,
                   std::vector<const llvm::Value *> &aliases) const;
  ExecutionState getExecutionState(const llvm::Instruction *instruction) const;
  bool isInAtomicContext(const llvm::Instruction *instruction) const;

  const std::map<LockID, LockInfo> &getLockInfoMap() const {
    return lock_info_map_;
  }

  std::vector<KernelOperation> findLockAcquiresWithoutRelease() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findPotentialDeadlocks() const;

  std::vector<const llvm::Instruction *> findDoubleLocks() const;

  std::vector<const llvm::Instruction *> findUnlockWithoutLock() const;

  std::vector<const llvm::Instruction *> findMixRawAndcooked() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findLockOrderInversion() const;

  std::vector<const llvm::Instruction *> findRCUWithoutGracePeriod() const;

  std::vector<const llvm::Instruction *> findSleepInAtomic() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findUseAfterFree() const;

  std::vector<const llvm::Instruction *> findTimerIssues() const;

  std::vector<const llvm::Instruction *> findIrqSaveRestoreMismatch() const;

private:
  llvm::Module &module_;
  LinuxKernelConfig config_;
  LinuxKernelSemanticRegistry semantic_registry_;
  lotus::AliasAnalysisWrapper *alias_analysis_ = nullptr;

  std::vector<KernelOperation> all_operations_;
  std::unordered_map<OperationKind, size_t> operation_kind_counts_;

  std::map<LockID, LockInfo> lock_info_map_;
  std::map<RCUSyncPointID, RCUSection> rcu_sections_;
  std::map<WaitQueueID, WaitQueueEntry> wait_queue_entries_;

  std::map<std::pair<const llvm::Function *, LockID>, int> lock_depth_;
  std::unordered_map<const llvm::Function *, std::vector<size_t>>
      operations_by_function_;
  std::unordered_map<const llvm::Instruction *, std::vector<size_t>>
      operation_indices_by_inst_;
  std::unordered_map<const llvm::Instruction *, size_t> instruction_order_;
  mutable std::map<std::pair<const llvm::Value *, int64_t>, const llvm::Value *>
      canonical_pointer_ids_;

  OperationKind classifyOperation(const llvm::Instruction *inst,
                                  const llvm::StringRef &func_name) const;
  LockKind classifyLockKind(const llvm::StringRef &func_name) const;

  void applyConfiguredSemantics(KernelOperation &op,
                                const LinuxKernelAPISemantics &semantics);

  void trackLockState(KernelOperation &op);
  void analyzeLockUsage();

  bool maySleep(const llvm::Instruction *inst) const;
};

} // namespace kernel

#endif // LINUX_KERNEL_PROCESS_MODEL_H
