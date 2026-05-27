#include "Verification/PathProgram/PathProgram.h"

#include <functional>

namespace lotus::verification::pathprogram {

std::size_t
TraceTransitionHash::operator()(const TraceTransition &transition) const {
  const auto sourceHash =
      std::hash<const llvm::BasicBlock *>()(transition.source);
  const auto targetHash =
      std::hash<const llvm::BasicBlock *>()(transition.target);
  const auto indexHash = std::hash<unsigned>()(transition.successorIndex);
  return sourceHash ^ (targetHash << 1) ^ (indexHash << 2);
}

bool PathProgram::containsLocation(const llvm::BasicBlock &block) const {
  return locationSet_.count(&block) != 0;
}

bool PathProgram::containsTransition(const TraceTransition &transition) const {
  return transitionSet_.count(transition) != 0;
}

std::vector<TraceTransition>
PathProgram::outgoingTransitions(const llvm::BasicBlock &block) const {
  auto it = outgoing_.find(&block);
  if (it == outgoing_.end()) {
    return {};
  }
  return it->second;
}

std::vector<TraceTransition>
PathProgram::incomingTransitions(const llvm::BasicBlock &block) const {
  auto it = incoming_.find(&block);
  if (it == incoming_.end()) {
    return {};
  }
  return it->second;
}

} // namespace lotus::verification::pathprogram
