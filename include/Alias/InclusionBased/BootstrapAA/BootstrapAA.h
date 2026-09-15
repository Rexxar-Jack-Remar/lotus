#pragma once

#include "Alias/InclusionBased/BootstrapAA/Engine.h"

namespace llvm {
class CallBase;
class Function;
class Instruction;
class Module;
class Value;
} // namespace llvm

namespace lotus {
namespace bootstrap {

// LLVM 14 adapter. The module must outlive this object and remain unchanged.
// Results describe executions starting at entry (default: main), after global
// initialization. This is a sequential, closed-world analysis, not an LLVM AA
// pass: it requires a program point and, optionally, a matched call-site
// context.
class BootstrapAA {
public:
  using CallContext = std::vector<const llvm::CallBase *>;
  explicit BootstrapAA(const llvm::Module &module,
                       const llvm::Function *entry = nullptr,
                       Options options = {});
  ~BootstrapAA();
  BootstrapAA(BootstrapAA &&) noexcept;
  BootstrapAA &operator=(BootstrapAA &&) noexcept;
  BootstrapAA(const BootstrapAA &) = delete;
  BootstrapAA &operator=(const BootstrapAA &) = delete;

  QueryResult pointsTo(const llvm::Value &value, const llvm::Instruction &site,
                       const CallContext &context = {},
                       Point point = Point::Before);
  QueryResult pointsToAllContexts(const llvm::Value &value,
                                  const llvm::Instruction &site,
                                  Point point = Point::Before);
  bool mayAlias(const llvm::Value &lhs, const llvm::Value &rhs,
                const llvm::Instruction &site, const CallContext &context = {});

  // UNKNOWN and NULL_OBJECT have no LLVM allocation site. A function object
  // maps to its Function, a global to its GlobalVariable, and storage to its
  // allocating AllocaInst/CallBase. INVALID means the value is not an
  // allocation site.
  const llvm::Value *allocationSite(Id object) const;
  Id objectId(const llvm::Value &allocation) const;
  const Object &objectInfo(Id object) const;
  const Statistics &statistics() const;
  const SteensgaardHierarchy &hierarchy() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace bootstrap
} // namespace lotus
