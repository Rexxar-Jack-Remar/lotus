#pragma once

#include "llvm/IR/Module.h"

#include <string>
#include <vector>

namespace pdg {
struct TaintFlowResult;

struct ArithmeticRuleDescriptor {
  std::string id;
  std::string severity;
  std::string codeql_query;
  std::string coverage;
};

struct ArithmeticFinding {
  std::string rule_id;
  const llvm::Instruction *site = nullptr;
  std::string message;
  std::vector<const llvm::Instruction *> evidence;
};

struct ArithmeticQueryResult {
  std::vector<ArithmeticFinding> findings;
};

/// Numeric rules use mathematical intervals before machine-width truncation,
/// dominating CFG guards, and must-alias MemorySSA definitions. Taint is
/// supplied by PDG's own value flow service; unknown values are never taint
/// sources.
class ArithmeticQuery {
public:
  static const std::vector<ArithmeticRuleDescriptor> &catalog();
  static bool requiresTaint(const std::string &id);
  ArithmeticQueryResult analyze(const llvm::Module &module,
                                const TaintFlowResult *taint = nullptr) const;
};
} // namespace pdg
