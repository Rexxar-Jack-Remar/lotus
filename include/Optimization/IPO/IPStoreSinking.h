//===- IPStoreSinking.h - Inter-procedural Store Sinking ------------------===//
//
// Conservative store sinking that moves store instructions closer to their
// first observable use within a basic block. Only sinks past side-effect-free
// instructions (shadow.mem bookkeeping calls are transparent to the check).
//
// Pass name: "ip-sink"
//===----------------------------------------------------------------------===//

#pragma once

namespace llvm {
class ModulePass;
} // namespace llvm

namespace previrt {
namespace transforms {

/// Create an inter-procedural Store Sinking pass.
///
/// The pass is also self-registered under the name "ip-sink" and can be
/// invoked directly through the LLVM pass manager pipeline.
llvm::ModulePass *createIPStoreSinkingPass();

} // namespace transforms
} // namespace previrt
