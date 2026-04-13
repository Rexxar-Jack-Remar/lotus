//===- IPRedundantLoadElimination.h - Inter-proc. Redundant Load Elim. ----===//
//
// Conservative inter-procedural redundant load elimination using MemorySSA.
//
// Removes repeated loads from the same pointer within a basic block when the
// MemorySSA version (TLVar) and pointer operand are identical and no
// intervening real memory write occurs. Interprocedural effects are already
// encoded in the TLVars produced by ShadowMem.
//
// Pass name: "ip-rle"
//===----------------------------------------------------------------------===//

#pragma once

namespace llvm {
class ModulePass;
} // namespace llvm

namespace previrt {
namespace transforms {

/// Create an inter-procedural Redundant Load Elimination pass.
///
/// The pass is also self-registered under the name "ip-rle" and can be
/// invoked directly through the LLVM pass manager pipeline.
llvm::ModulePass *createIPRedundantLoadEliminationPass();

} // namespace transforms
} // namespace previrt
