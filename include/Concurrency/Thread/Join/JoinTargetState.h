#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Instruction.h>

namespace mhp {

enum class ThreadExecutionClass : uint8_t {
  SingleExecution = 0,
  RepeatedExecution = 1,
};

enum class ThreadLifecycle : uint8_t {
  None = 0,
  Joinable = 1 << 0,
  Joined = 1 << 1,
  Detached = 1 << 2,
  Overwritten = 1 << 3,
  Escaped = 1 << 4,
  Failed = 1 << 5,
};

struct ThreadInstance {
  const llvm::Instruction *fork_site = nullptr;
  uint8_t epoch_class = 0;
  ThreadExecutionClass execution_class = ThreadExecutionClass::SingleExecution;

  bool operator==(const ThreadInstance &other) const {
    return fork_site == other.fork_site && epoch_class == other.epoch_class &&
           execution_class == other.execution_class;
  }
};

struct ThreadInstanceHash {
  std::size_t operator()(const ThreadInstance &instance) const;
};

struct InstanceState {
  uint8_t lifecycle_mask = static_cast<uint8_t>(ThreadLifecycle::None);
  bool creation_may_fail = false;
  bool join_may_fail = false;

  bool operator==(const InstanceState &other) const {
    return lifecycle_mask == other.lifecycle_mask &&
           creation_may_fail == other.creation_may_fail &&
           join_may_fail == other.join_may_fail;
  }

  bool mayBeJoinable() const {
    return (lifecycle_mask & static_cast<uint8_t>(ThreadLifecycle::Joinable)) !=
           0;
  }

  bool hasLifecycle(ThreadLifecycle lifecycle) const {
    return (lifecycle_mask & static_cast<uint8_t>(lifecycle)) != 0;
  }
};

struct HandleLocation {
  const llvm::Value *base = nullptr;
  std::vector<int64_t> offsets;
  bool is_base_wildcard = false;

  bool operator==(const HandleLocation &other) const {
    return base == other.base && offsets == other.offsets &&
           is_base_wildcard == other.is_base_wildcard;
  }
};

struct HandleLocationHash {
  std::size_t operator()(const HandleLocation &location) const;
};

struct SummaryLocation {
  unsigned arg_no = 0;
  std::vector<int64_t> offsets;
  bool is_base_wildcard = false;

  bool operator==(const SummaryLocation &other) const {
    return arg_no == other.arg_no && offsets == other.offsets &&
           is_base_wildcard == other.is_base_wildcard;
  }
};

struct SummaryLocationHash {
  std::size_t operator()(const SummaryLocation &location) const;
};

struct HandleState {
  std::unordered_map<ThreadInstance, InstanceState, ThreadInstanceHash>
      instances;
  std::unordered_set<unsigned> preserved_arg_inputs;
  std::unordered_set<const llvm::Value *> provenance_roots;
  bool has_unknown_live_fork = false;
  bool path_sensitivity_lost = false;

  bool operator==(const HandleState &other) const {
    return has_unknown_live_fork == other.has_unknown_live_fork &&
           path_sensitivity_lost == other.path_sensitivity_lost &&
           instances == other.instances &&
           preserved_arg_inputs == other.preserved_arg_inputs &&
           provenance_roots == other.provenance_roots;
  }
};

struct FunctionSummary {
  std::unordered_map<SummaryLocation, HandleState, SummaryLocationHash>
      location_exit_states;
  std::unordered_map<HandleLocation, HandleState, HandleLocationHash>
      global_exit_states;
  HandleState return_state;
};

using JoinTargetStateMap =
    std::unordered_map<HandleLocation, HandleState, HandleLocationHash>;

} // namespace mhp
