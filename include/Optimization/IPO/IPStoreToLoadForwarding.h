//===- IPStoreToLoadForwarding.h - Inter-proc. Store-to-Load Forwarding ---===//
//
// Replaces load instructions with the value from a unique reaching store by
// walking MemorySSA def-use chains across function boundaries (BFS over
// shadow.mem.store, shadow.mem.arg.*, shadow.mem.in/out, and PHI nodes).
// The load is replaced only when exactly one reaching store value is found.
//
// Pass name: "ip-forward"
//===----------------------------------------------------------------------===//

#pragma once

namespace llvm {
class ModulePass;
} // namespace llvm

namespace previrt {
namespace transforms {

/// Create an inter-procedural Store-to-Load Forwarding pass.
///
/// The pass is also self-registered under the name "ip-forward" and can be
/// invoked directly through the LLVM pass manager pipeline.
llvm::ModulePass *createIPStoreToLoadForwardingPass();

} // namespace transforms
} // namespace previrt
