#pragma once

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"

#include <string>
#include <vector>

namespace pdg {

struct CallContractRuleDescriptor {
  const char *id;
  const char *severity;
  const char *codeql_query;
  const char *coverage;
};

struct CallContractFinding {
  std::string rule_id;
  const llvm::Instruction *site;
  std::string message;
  std::vector<const llvm::Instruction *> evidence;
};

struct CallContractQueryResult {
  std::vector<CallContractFinding> findings;
};

/// Contracts for promoted format arguments, resolved C call signatures, and
/// module-wide variadic sentinel conventions. Unsupported format directives or
/// unresolved callees remain unknown instead of guessing a source-level type.
class CallContractQuery {
public:
  static const std::vector<CallContractRuleDescriptor> &catalog();
  CallContractQueryResult analyze(const llvm::Module &module) const;
};

} // namespace pdg
