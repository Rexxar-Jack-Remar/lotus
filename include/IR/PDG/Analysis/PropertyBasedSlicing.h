#pragma once

#include "IR/PDG/Analysis/Slicing.h"

#include <set>
#include <string>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace pdg {

enum class PropertyKind {
  UnreachCall,
  Assertions,
  MemSafety,
  NoOverflow,
  Termination,
  CoverageErrorCall,
  CoverageBranches,
  CoverageStatements,
  CoverageConditions,
  NullDeref,
  DefBehavior,
  MemCleanup,
  Unknown
};

enum class PropertyType {
  CHECK,  // Safety property
  COVER   // Coverage property
};

struct PropertyRule {
  PropertyType type = PropertyType::CHECK;
  PropertyKind kind = PropertyKind::Unknown;
  std::string target;  // For call() targets
  bool negated = false; // For ! operator in LTL
};

class PropertySpec {
public:
  static bool parseFromFile(const std::string &path, PropertySpec &out,
                            std::string &error);
  static bool parseFromString(const std::string &content, PropertySpec &out,
                              std::string &error);

  const std::vector<PropertyRule> &rules() const { return _rules; }
  bool empty() const { return _rules.empty(); }
  PropertyType getType() const { return _type; }

private:
  std::vector<PropertyRule> _rules;
  PropertyType _type = PropertyType::CHECK;
  
  friend class PropertyParser;
};

class PropertyBasedSlicing {
public:
  using NodeSet = std::set<Node *>;

  explicit PropertyBasedSlicing(GenericGraph &pdg) : _pdg(pdg) {}

  NodeSet resolveCriteria(const llvm::Module &M, const PropertySpec &spec) const;
  NodeSet computeBackwardSlice(const llvm::Module &M,
                               const PropertySpec &spec) const;
  NodeSet computeForwardSlice(const llvm::Module &M,
                              const PropertySpec &spec) const;

private:
  GenericGraph &_pdg;
};

} // namespace pdg
