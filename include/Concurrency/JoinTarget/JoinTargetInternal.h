#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Instruction.h>

namespace mhp {

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
  std::unordered_set<const llvm::Instruction *> live_forks;
  std::unordered_set<const llvm::Instruction *> historical_forks;
  std::unordered_set<unsigned> preserved_arg_inputs;
  bool has_unknown_live_fork = false;

  bool operator==(const HandleState &other) const {
    return has_unknown_live_fork == other.has_unknown_live_fork &&
           live_forks == other.live_forks &&
           historical_forks == other.historical_forks &&
           preserved_arg_inputs == other.preserved_arg_inputs;
  }
};

struct FunctionSummary {
  std::unordered_map<SummaryLocation, HandleState, SummaryLocationHash>
      location_exit_states;
};

using JoinTargetStateMap =
    std::unordered_map<HandleLocation, HandleState, HandleLocationHash>;

} // namespace mhp
