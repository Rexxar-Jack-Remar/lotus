#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace llvm {
class Instruction;
class Module;
} // namespace llvm

namespace pdg {

struct LifetimeRuleDescriptor {
  std::string id;
  std::string severity;
  std::string codeql_query;
  std::string coverage;
};

struct LifetimeFinding {
  std::string rule_id;
  std::string message;
  const llvm::Instruction *site = nullptr;
  std::vector<const llvm::Instruction *> evidence;
};

struct LifetimeQueryResult {
  std::vector<LifetimeFinding> findings;
  /// Objects whose exploration exceeded the limit, or re-entered their
  /// allocation site. No leak conclusion is emitted for these objects.
  size_t incomplete_objects = 0;
  size_t budget_limited_objects = 0;
  bool state_limit_hit = false;
};

/// PDG semantic support for allocation identity and resource lifecycle. Uses
/// LLVM CFG/SSA and exact local stack locations directly, with no bug-engine,
/// IFDS, or graph-reachability-as-lifetime dependency. Alternatives retain
/// separate release state; overwrites kill local aliases. Unknown ownership
/// effects escape the object. The supported branch facts are Boolean identity
/// and allocation failure checks, not general SMT path feasibility.
class LifetimeQuery {
public:
  static const std::vector<LifetimeRuleDescriptor> &catalog();
  /// A zero budget permits unlimited exploration. Incomplete objects still
  /// report locally witnessed double frees/accesses, but never leak claims.
  LifetimeQueryResult analyze(const llvm::Module &module,
                              size_t max_states_per_object = 4096) const;
};

} // namespace pdg
