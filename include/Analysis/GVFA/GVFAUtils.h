#ifndef ANALYSIS_GVFA_GVFAUTILS_H
#define ANALYSIS_GVFA_GVFAUTILS_H

#include "Alias/DyckAA/DyckVFG.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

using namespace llvm;

// Hash function for pair<const CallInst *, const Function *>
namespace std {
template <> struct hash<std::pair<const CallInst *, const Function *>> {
  size_t
  operator()(const std::pair<const CallInst *, const Function *> &p) const {
    return std::hash<const CallInst *>()(p.first) ^
           (std::hash<const Function *>()(p.second) << 1);
  }
};
} // namespace std

namespace GVFAUtils {

//===----------------------------------------------------------------------===//
// VFG Navigation Utilities
//===----------------------------------------------------------------------===//

/**
 * Gets all successors of a value in the Value Flow Graph.
 *
 * @param V The value to get successors for
 * @param VFG The Value Flow Graph
 * @return A vector of successor values
 */
std::vector<const Value *> getSuccessors(const Value *V, DyckVFG *VFG);

/**
 * Gets all predecessors of a value in the Value Flow Graph.
 *
 * @param V The value to get predecessors for
 * @param VFG The Value Flow Graph
 * @return A vector of predecessor values
 */
std::vector<const Value *> getPredecessors(const Value *V, DyckVFG *VFG);

/**
 * Checks if there is a direct value flow edge between two values.
 *
 * @param From The source value
 * @param To The target value
 * @param VFG The Value Flow Graph
 * @return true if there is a direct edge from From to To
 */
bool isValueFlowEdge(const Value *From, const Value *To, DyckVFG *VFG);

//===----------------------------------------------------------------------===//
// Call Site Management Utilities
//===----------------------------------------------------------------------===//

/**
 * Gets or assigns a unique ID for a call site.
 *
 * @param CI The call instruction
 * @param CallSiteIndexMap The map maintaining call site IDs
 * @return The unique ID for this call site
 */
int getCallSiteID(const CallInst *CI,
                  std::unordered_map<const CallInst *, int> &CallSiteIndexMap);

/**
 * Gets or assigns a unique ID for a call site and callee pair.
 *
 * @param CI The call instruction
 * @param Callee The called function
 * @param CallSiteCalleePairIndexMap The map maintaining call site-callee pair
 * IDs
 * @return The unique ID for this call site-callee pair
 */
int getCallSiteID(
    const CallInst *CI, const Function *Callee,
    std::unordered_map<std::pair<const CallInst *, const Function *>, int>
        &CallSiteCalleePairIndexMap);

//===----------------------------------------------------------------------===//
// Witness Path Extraction Utilities
//===----------------------------------------------------------------------===//

/**
 * Extract a witness path from source to target showing key propagation steps.
 *
 * Returns a vector of key intermediate values (stores, loads, calls, returns,
 * PHIs) that demonstrate how the value flows from source to target.
 *
 * @param From The source value
 * @param To The target value
 * @param VFG The Value Flow Graph
 * @return A vector of values representing the witness path
 */
std::vector<const Value *> getWitnessPath(const Value *From, const Value *To,
                                          DyckVFG *VFG);

/**
 * Extract a witness path using detailed reachability information for guidance.
 *
 * Uses AllReachabilityMap to only explore values that are known to be reachable
 * from the source, making the search more efficient and accurate.
 *
 * @param From The source value
 * @param To The target value
 * @param VFG The Value Flow Graph
 * @param AllReachabilityMap Map from values to the set of sources that reach
 * them
 * @return A vector of values representing the witness path
 */
std::vector<const Value *> getWitnessPathGuided(
    const Value *From, const Value *To, DyckVFG *VFG,
    const std::unordered_map<const Value *, std::unordered_set<const Value *>>
        &AllReachabilityMap);

} // namespace GVFAUtils

#endif // ANALYSIS_GVFA_GVFAUTILS_H
