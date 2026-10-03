#pragma once

#include "IR/SVFG/SVFG.h"
#include "IR/ICFG/ICFG.h"

namespace llvm {
class Module;
}

namespace lotus {
namespace analysis {

/// Pair free and dereference sites using SVFG points-to facts, then recover
/// their order with a bounded-context ICFG walk. Reports are path-insensitive
/// candidates; branch feasibility is not checked by SMT.
class UseAfterFreeChecker {
public:
  void runOnModule(const llvm::Module &module, const SVFG &svfg, ICFG &icfg) const;
};

} // namespace analysis
} // namespace lotus
