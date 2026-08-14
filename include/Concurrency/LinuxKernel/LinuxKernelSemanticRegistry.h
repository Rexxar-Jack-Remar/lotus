/**
 * @file LinuxKernelSemanticRegistry.h
 * @brief Declarative Linux kernel API semantic registry.
 */

#pragma once

#include "Concurrency/LinuxKernel/LinuxKernelOperation.h"

#include <string>
#include <vector>

#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>

namespace kernel {

struct LinuxKernelConfig;

struct LinuxKernelAPISemantics {
  enum class MatchKind { EXACT, PREFIX };
  static constexpr int NoOperand = -1;
  static constexpr int ReturnValue = -2;

  std::string pattern;
  MatchKind match = MatchKind::EXACT;
  OperationKind operation = OperationKind::UNKNOWN;
  LockKind lock_kind = LockKind::UNKNOWN;
  LockMode lock_mode = LockMode::UNKNOWN;
  ConditionalSuccess success = ConditionalSuccess::UNCONDITIONAL;
  AsyncContextKind async_context = AsyncContextKind::NONE;
  AsyncContextKind secondary_async_context = AsyncContextKind::NONE;
  RCUFlavor rcu_flavor = RCUFlavor::UNKNOWN;
  CompletionSignalKind completion_signal = CompletionSignalKind::UNKNOWN;
  KernelMemoryOrder memory_order = KernelMemoryOrder::UNKNOWN;

  int object_arg = NoOperand;
  int callback_arg = NoOperand;
  int secondary_callback_arg = NoOperand;
  int domain_arg = NoOperand;
  int condition_arg = NoOperand;
  int flags_arg = NoOperand;
  int value_arg = NoOperand;
  int size_arg = NoOperand;
  int expires_arg = NoOperand;
  int subclass_arg = NoOperand;

  bool synchronous = false;
  bool serializes_domain = false;
  bool raw_lock = false;
  bool nested_lock = false;
  bool interruptible = false;
  bool timeout = false;
  bool wake_all = false;
  bool wake_exclusive = false;
  bool deferred_reclamation = false;
  bool returns_retired_pointer = false;
  bool requires_rcu_section = true;
  bool managed_allocation = false;
  bool may_sleep = false;
  bool may_spawn = false;
  bool may_access_shared_memory = false;
  bool saves_irq_state = false;
  bool restores_irq_state = false;
  bool disables_local_irq = false;
  bool enables_local_irq = false;
  bool disables_bh = false;
  bool enables_bh = false;
  bool disables_preemption = false;
  bool enables_preemption = false;
  bool preemption_effect_non_rt = false;
  std::string source;
};

class LinuxKernelSemanticRegistry {
public:
  void load(const LinuxKernelConfig &config);
  bool loadFile(llvm::StringRef path);

  const LinuxKernelAPISemantics *lookup(llvm::StringRef symbol) const;
  const std::vector<std::string> &getLoadedFiles() const {
    return loaded_files_;
  }
  const std::vector<std::string> &getErrors() const { return errors_; }

private:
  llvm::StringMap<LinuxKernelAPISemantics> exact_;
  std::vector<LinuxKernelAPISemantics> prefixes_;
  std::vector<std::string> loaded_files_;
  std::vector<std::string> errors_;

  void add(LinuxKernelAPISemantics semantics);
};

} // namespace kernel
