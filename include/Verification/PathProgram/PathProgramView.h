#pragma once

#include "Verification/PathProgram/PathProgram.h"

namespace lotus::verification::pathprogram {

class PathProgramView {
public:
  explicit PathProgramView(const PathProgram &program) : program_(program) {}

  llvm::ArrayRef<const llvm::BasicBlock *> locations() const {
    return program_.locations();
  }

  llvm::ArrayRef<TraceTransition> transitions() const {
    return program_.transitions();
  }

  llvm::ArrayRef<TraceTransition> pathTransitions() const {
    return program_.pathTransitions();
  }

private:
  const PathProgram &program_;
};

} // namespace lotus::verification::pathprogram
