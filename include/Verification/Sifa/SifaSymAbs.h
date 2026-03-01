//===-- Verification/Sifa/SifaSymAbs.h ------------------------------------===//
//
// Public API for Sifa using SymbolicAbstraction-backed abstract domains.
//
// This provides support for domains such as Interval and Octagon by using
// SymbolicAbstraction as the transfer engine for LLVM CFG edges. Call handling
// comes from SymbolicAbstraction's own transformers and ModuleContext rather
// than Sifa's interprocedural worklist/call-summary machinery.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SIFASYMABS_H
#define LOTUS_VERIFICATION_SIFA_SIFASYMABS_H

#include "Verification/Sifa/SymAbs/SifaSymAbsOptions.h"

#include <memory>

namespace llvm {
class BasicBlock;
class Function;
class Module;
} // namespace llvm

namespace symbolic_abstraction {
class AbstractValue;
} // namespace symbolic_abstraction

namespace lotus {
namespace sifa {

using SymAbsState = std::shared_ptr<symbolic_abstraction::AbstractValue>;

/// Run Sifa for a single function and compute the abstract state at `target`
/// (after phi nodes in `target`).
///
/// Returns a null state for bottom/unreachable.
SymAbsState analyzeSymAbsTo(const llvm::Module &M, const llvm::Function &F,
                            const llvm::BasicBlock &target,
                            const SifaSymAbsOptions &options = {});

/// Convenience wrapper: reachability query using the selected abstract domain.
bool isReachableSymAbs(const llvm::Module &M, const llvm::Function &F,
                       const llvm::BasicBlock &target,
                       const SifaSymAbsOptions &options = {});

/// Compute the abstract state at the procedure exit (the synthetic EXIT node).
SymAbsState analyzeSymAbsToReturn(const llvm::Module &M, const llvm::Function &F,
                                  const SifaSymAbsOptions &options = {});

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SIFASYMABS_H
