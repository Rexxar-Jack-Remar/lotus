//===- IPDeadStoreElimination.h - Inter-procedural Dead Store Elimination --===//
//
// Inter-procedural Dead Store Elimination (IP-DSE) using ShadowMem/MemorySSA.
//
// Removes store instructions and global initializers whose MemorySSA def-use
// chains never reach a shadow.mem.load, walking across function boundaries via
// shadow.mem.arg.* and shadow.mem.in/out.
//
// Requires: SeaDSA's ShadowMemPass (instruments the module with shadow.mem
//           calls before this pass runs).
//
// Pass name: "ipdse"
//===----------------------------------------------------------------------===//

#pragma once

namespace llvm {
class ModulePass;
} // namespace llvm

namespace previrt {
namespace transforms {

/// Create an inter-procedural Dead Store Elimination pass.
///
/// The pass is also self-registered under the name "ipdse" and can be
/// invoked directly through the LLVM pass manager pipeline.
llvm::ModulePass *createIPDeadStoreEliminationPass();

} // namespace transforms
} // namespace previrt
