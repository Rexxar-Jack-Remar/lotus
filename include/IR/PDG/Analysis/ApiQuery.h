#pragma once

#include <string>
#include <vector>

namespace llvm {
class Instruction;
class Module;
} // namespace llvm

namespace pdg {

struct ApiRuleDescriptor {
  std::string id;
  std::string severity;
  std::string codeql_query;
  std::string coverage;
};

struct ApiFinding {
  std::string rule_id;
  const llvm::Instruction *site;
  std::string message;
  std::vector<const llvm::Instruction *> evidence;
};

struct ApiQueryResult {
  std::vector<ApiFinding> findings;
};

/// Library security protocols evaluated over LLVM values and executable CFG
/// paths. API spelling identifies a model; reporting requires its argument,
/// configuration state, or result-use precondition.
class ApiQuery {
public:
  static const std::vector<ApiRuleDescriptor> &catalog();
  ApiQueryResult analyze(const llvm::Module &module) const;
};

} // namespace pdg
