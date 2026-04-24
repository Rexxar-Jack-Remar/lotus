#pragma once

#include "Alias/InclusionBased/TPA/Context/Context.h"
#include "Alias/InclusionBased/TPA/Context/ProgramPoint.h"

#include <unordered_set>

namespace context {

// Adaptive context sensitivity implementation
//
// Adaptive context sensitivity selectively tracks important call sites while
// discarding less critical ones. This balances precision and scalability
// by only distinguishing contexts at call sites that are deemed important.
//
// Design:
// - Maintains a set of tracked call sites that are considered "important"
// - When entering a tracked call site, a new context is created
// (context-sensitive)
// - When entering an untracked call site, the current context is reused
// (context-insensitive)
// - This reduces context explosion for large programs while preserving
// precision where it matters
//
// Use Cases:
// - Programs with many small helper functions that don't need context
// distinction
// - Recursive functions where tracking all contexts would explode
// - Library functions called from many different contexts
//
// Example:
//   If we track calls to "critical_function" but not to "helper_function":
//   - main() -> helper() -> critical() creates new context at critical()
//   - main2() -> helper() -> critical() creates new context at critical()
//   - These are different contexts, enabling precise analysis of
//   critical_function
//   - But helper() calls don't create contexts, reducing analysis cost
class AdaptiveContext {
private:
  // Set of call sites that are tracked with context sensitivity
  // These call sites will create new contexts when entered
  // Call sites not in this set are treated context-insensitively
  static std::unordered_set<ProgramPoint> trackedCallsites;

public:
  // Mark a call site as important and to be tracked
  // Subsequent calls through this program point will create new contexts
  // Parameters: pp - the program point (context + CFG node) to track
  static void trackCallSite(const ProgramPoint &);

  // Push a new call site onto the context stack with adaptive tracking
  // If the call site is in trackedCallsites, creates a new context
  // Otherwise, returns the current context unchanged (context-insensitive)
  // Parameters: ctx - the current context before the call, inst - the call
  // instruction
  static const Context *pushContext(const Context *, const llvm::Instruction *);
  // Push a new call site using a ProgramPoint (contains both context and node)
  static const Context *pushContext(const ProgramPoint &);
};

} // namespace context
