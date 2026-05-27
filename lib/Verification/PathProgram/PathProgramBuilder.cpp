#include "Verification/PathProgram/PathProgramBuilder.h"

#include "llvm/IR/CFG.h"
#include "llvm/Support/Error.h"

#include <cerrno>
#include <string>

namespace lotus::verification::pathprogram {

namespace {

llvm::Error invalidPathError(const std::string &message) {
  return llvm::createStringError(
      std::make_error_code(std::errc::invalid_argument), "%s",
      message.c_str());
}

bool isBlockInFunction(const llvm::Function &function,
                       const llvm::BasicBlock *block) {
  if (block == nullptr) {
    return false;
  }
  return block->getParent() == &function;
}

bool isValidTransition(const llvm::Function &function,
                       const TraceTransition &transition) {
  if (!isBlockInFunction(function, transition.source) ||
      !isBlockInFunction(function, transition.target)) {
    return false;
  }

  unsigned index = 0;
  for (const llvm::BasicBlock *successor : llvm::successors(transition.source)) {
    if (index == transition.successorIndex) {
      return successor == transition.target;
    }
    ++index;
  }
  return false;
}

} // namespace

llvm::Expected<PathProgram>
PathProgramBuilder::build(const llvm::Function &function,
                          llvm::ArrayRef<TraceTransition> transitionTrace) {
  if (transitionTrace.empty()) {
    return invalidPathError("transition trace must not be empty");
  }

  if (transitionTrace.front().source != &function.getEntryBlock()) {
    return invalidPathError(
        "transition trace must start at the function entry block");
  }

  PathProgram program;
  program.function_ = &function;
  program.entryLocation_ = transitionTrace.front().source;
  program.exitLocation_ = transitionTrace.back().target;

  auto addLocation = [&](const llvm::BasicBlock *block) {
    if (program.locationSet_.insert(block).second) {
      program.locations_.push_back(block);
    }
  };

  auto addTransition = [&](const TraceTransition &transition) {
    if (program.transitionSet_.insert(transition).second) {
      program.transitions_.push_back(transition);
      program.outgoing_[transition.source].push_back(transition);
      program.incoming_[transition.target].push_back(transition);
    }
  };

  const llvm::BasicBlock *expectedSource = &function.getEntryBlock();
  for (const TraceTransition &transition : transitionTrace) {
    if (!isValidTransition(function, transition)) {
      return invalidPathError(
          "transition trace contains an invalid CFG transition");
    }
    if (transition.source != expectedSource) {
      return invalidPathError(
          "transition trace is not a connected path");
    }

    addLocation(transition.source);
    addLocation(transition.target);
    addTransition(transition);
    program.pathTransitions_.push_back(transition);
    expectedSource = transition.target;
  }

  return program;
}

} // namespace lotus::verification::pathprogram
