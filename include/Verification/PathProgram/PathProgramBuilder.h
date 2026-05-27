#pragma once

#include "Verification/PathProgram/PathProgram.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

namespace lotus::verification::pathprogram {

class PathProgramBuilder {
public:
  /// Build a path program from a concrete transition trace.
  ///
  /// The trace must start at the function entry and each transition must be a
  /// valid CFG edge in the given function.
  static llvm::Expected<PathProgram>
  build(const llvm::Function &function,
        llvm::ArrayRef<TraceTransition> transitionTrace);
};

} // namespace lotus::verification::pathprogram
