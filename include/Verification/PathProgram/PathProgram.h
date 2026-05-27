#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::verification::pathprogram {

/// A control-flow transition in an LLVM CFG trace.
///
/// This is the Lotus v1 representation of a paper transition (l, rho, l').
/// We model the control component directly as a CFG edge and distinguish
/// different outgoing edges from the same terminator using successorIndex.
struct TraceTransition {
  const llvm::BasicBlock *source = nullptr;
  const llvm::BasicBlock *target = nullptr;
  unsigned successorIndex = 0;

  bool operator==(const TraceTransition &other) const {
    return source == other.source && target == other.target &&
           successorIndex == other.successorIndex;
  }
};

struct TraceTransitionHash {
  std::size_t operator()(const TraceTransition &transition) const;
};

/// A path program induced by a concrete path, aligned with PLDI'07 Path
/// Invariants at the control-flow level:
/// - locations are the program locations visited by the path
/// - transitions are exactly the transitions that occur in the path
///
/// The ordered path is preserved separately as pathTransitions().
class PathProgram {
public:
  const llvm::Function &function() const { return *function_; }

  llvm::ArrayRef<const llvm::BasicBlock *> locations() const {
    return locations_;
  }

  llvm::ArrayRef<TraceTransition> transitions() const { return transitions_; }

  llvm::ArrayRef<TraceTransition> pathTransitions() const {
    return pathTransitions_;
  }

  const llvm::BasicBlock *entryLocation() const { return entryLocation_; }
  const llvm::BasicBlock *exitLocation() const { return exitLocation_; }

  bool containsLocation(const llvm::BasicBlock &block) const;
  bool containsTransition(const TraceTransition &transition) const;

  std::vector<TraceTransition>
  outgoingTransitions(const llvm::BasicBlock &block) const;

  std::vector<TraceTransition>
  incomingTransitions(const llvm::BasicBlock &block) const;

private:
  friend class PathProgramBuilder;

  const llvm::Function *function_ = nullptr;
  const llvm::BasicBlock *entryLocation_ = nullptr;
  const llvm::BasicBlock *exitLocation_ = nullptr;
  std::vector<const llvm::BasicBlock *> locations_;
  std::vector<TraceTransition> transitions_;
  std::vector<TraceTransition> pathTransitions_;
  std::unordered_set<const llvm::BasicBlock *> locationSet_;
  std::unordered_set<TraceTransition, TraceTransitionHash> transitionSet_;
  std::unordered_map<const llvm::BasicBlock *, std::vector<TraceTransition>>
      outgoing_;
  std::unordered_map<const llvm::BasicBlock *, std::vector<TraceTransition>>
      incoming_;
};

} // namespace lotus::verification::pathprogram
