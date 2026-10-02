#pragma once

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <map>
#include <string>
#include <vector>

namespace pdg {

enum class PointerNullState { Unknown, Null, NonNull, Nullable };
enum class InitializationState {
  Unknown,
  Uninitialized,
  Initialized,
  MaybeUninitialized
};

struct PointerStateFact {
  const llvm::Value *pointer = nullptr;
  PointerNullState nullness = PointerNullState::Unknown;
  InitializationState pointee_initialization = InitializationState::Unknown;
};

struct StateRuleDescriptor {
  std::string id;
  std::string severity;
  std::string codeql_query;
  std::string coverage;
};

struct StateFinding {
  std::string rule_id;
  std::string message;
  const llvm::Instruction *site = nullptr;
  std::vector<const llvm::Instruction *> evidence;
};

struct StateQueryResult {
  std::vector<StateFinding> findings;
  /// Facts immediately before pointer operands are used. A missing observation
  /// is unknown, not a proof of initialization or nonnullness.
  std::map<const llvm::Instruction *, std::vector<PointerStateFact>> states;
  size_t analyzed_functions = 0;
  bool convergence_limit_hit = false;

  PointerStateFact stateBefore(const llvm::Instruction &instruction,
                               const llvm::Value &pointer) const;
};

/// PDG-native intraprocedural state service over the retained LLVM CFG.
/// Tracks exact scalar stack cells, strong stores, predecessor joins, and
/// pointer-null branch refinements. Unknown writes and escaping cells become
/// unknown. Direct-call summaries describe null returns and parameter reads;
/// dependence reachability is never interpreted as definite initialization.
class StateQuery {
public:
  static const std::vector<StateRuleDescriptor> &catalog();
  StateQueryResult analyze(const llvm::Module &module,
                           const std::vector<std::string> &rule_ids = {}) const;
};

} // namespace pdg
