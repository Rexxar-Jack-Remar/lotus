#pragma once

#include "Alias/InclusionBased/TPA/Context/Context.h"

namespace llvm {
class Instruction;
} // namespace llvm

namespace context {

/// Context sensitivity strategy for TPA.
/// - KLimit: k-CFA at every call (current behavior).
/// - Selective: 0-CFA at direct (static) calls, k-CFA at indirect calls.
enum class ContextStrategy {
  KLimit,   /// k-CFA at all call sites
  Selective /// 0-CFA at direct calls, k-CFA at indirect calls
};

/// Set the active context strategy (must be set before running analysis).
void setContextStrategy(ContextStrategy s);

/// Get the current context strategy.
ContextStrategy getContextStrategy();

/// Push context for a call site according to the active strategy.
/// - KLimit: always pushes (with k-limiting).
/// - Selective: reuses current context for direct calls, pushes with k-limiting
///   for indirect calls.
/// @return New context for the callee (may be same as \p ctx for 0-CFA).
const Context *pushContextForCall(const Context *ctx,
                                  const llvm::Instruction *callInst);

} // namespace context
