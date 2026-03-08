/**
 * @file GVFAUtils.cpp
 * @brief Utility functions for Global Value Flow Analysis
 *
 * Contains VFG navigation helpers, call site management, and witness path
 * extraction.
 */

#include "Analysis/GVFA/GVFAUtils.h"

#include <algorithm>
#include <queue>
#include <unordered_set>

#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace GVFAUtils {

//===----------------------------------------------------------------------===//
// VFG Navigation Utilities
//===----------------------------------------------------------------------===//

std::vector<const Value *> getSuccessors(const Value *V, DyckVFG *VFG) {
  std::vector<const Value *> Successors;
  if (auto *Node = VFG->getVFGNode(const_cast<Value *>(V))) {
    Successors.reserve(std::distance(Node->begin(), Node->end()));
    for (auto It = Node->begin(); It != Node->end(); ++It) {
      Successors.push_back(It->first->getValue());
    }
  }
  return Successors;
}

std::vector<const Value *> getPredecessors(const Value *V, DyckVFG *VFG) {
  std::vector<const Value *> Predecessors;
  if (auto *Node = VFG->getVFGNode(const_cast<Value *>(V))) {
    Predecessors.reserve(std::distance(Node->in_begin(), Node->in_end()));
    for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
      Predecessors.push_back(It->first->getValue());
    }
  }
  return Predecessors;
}

bool isValueFlowEdge(const Value *From, const Value *To, DyckVFG *VFG) {
  if (auto *Node = VFG->getVFGNode(const_cast<Value *>(From))) {
    for (auto It = Node->begin(); It != Node->end(); ++It) {
      if (It->first->getValue() == To) {
        return true;
      }
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Call Site Management Utilities
//===----------------------------------------------------------------------===//

int getCallSiteID(const CallInst *CI,
                  std::unordered_map<const CallInst *, int> &CallSiteIndexMap) {
  auto It = CallSiteIndexMap.find(CI);
  if (It == CallSiteIndexMap.end()) {
    int ID = CallSiteIndexMap.size() + 1;
    CallSiteIndexMap[CI] = ID;
    return ID;
  } else {
    return It->second;
  }
}

int getCallSiteID(
    const CallInst *CI, const Function *Callee,
    std::unordered_map<std::pair<const CallInst *, const Function *>, int>
        &CallSiteCalleePairIndexMap) {
  std::pair<const CallInst *, const Function *> key =
      std::make_pair(CI, Callee);
  auto It = CallSiteCalleePairIndexMap.find(key);
  if (It == CallSiteCalleePairIndexMap.end()) {
    int ID = CallSiteCalleePairIndexMap.size() + 1;
    CallSiteCalleePairIndexMap[key] = ID;
    return ID;
  } else {
    return It->second;
  }
}

//===----------------------------------------------------------------------===//
// Witness Path Extraction Utilities
//===----------------------------------------------------------------------===//

std::vector<const Value *> getWitnessPath(const Value *From, const Value *To,
                                          DyckVFG *VFG) {
  std::vector<const Value *> path;

  // Sanity checks
  if (!From || !To || !VFG) {
    return path;
  }

  // BFS to find a path from From to To
  std::queue<const Value *> worklist;
  std::unordered_map<const Value *, const Value *> parent;
  std::unordered_set<const Value *> visited;

  worklist.push(From);
  visited.insert(From);
  parent[From] = nullptr;

  // Limit BFS to avoid infinite loops or very long searches
  const size_t MAX_ITERATIONS = 10000;
  size_t iterations = 0;

  bool found = false;
  while (!worklist.empty() && !found && iterations++ < MAX_ITERATIONS) {
    const Value *current = worklist.front();
    worklist.pop();

    if (current == To) {
      found = true;
      break;
    }

    // Get successors through VFG
    std::vector<const Value *> succs = getSuccessors(current, VFG);
    for (const Value *succ : succs) {
      if (!succ)
        continue; // Skip null successors

      if (visited.find(succ) == visited.end()) {
        visited.insert(succ);
        parent[succ] = current;
        worklist.push(succ);

        if (succ == To) {
          found = true;
          break;
        }
      }
    }
  }

  if (!found) {
    return path; // Empty path if not reachable
  }

  // Reconstruct path from parent pointers
  std::vector<const Value *> fullPath;
  const Value *current = To;
  while (current != nullptr) {
    fullPath.push_back(current);
    current = parent[current];
  }
  std::reverse(fullPath.begin(), fullPath.end());

  // Filter to keep only interesting propagation points:
  // - StoreInst: value is stored to memory
  // - LoadInst: value is loaded from memory
  // - CallInst: value is passed as argument or is result
  // - ReturnInst: value is returned from function
  // - PHINode: value flows through control flow merge
  // - GetElementPtrInst: pointer arithmetic
  // - Always include source and sink

  for (const Value *V : fullPath) {
    if (V == From || V == To) {
      // Always include source and sink
      path.push_back(V);
    } else if (const Instruction *I = dyn_cast<Instruction>(V)) {
      if (isa<StoreInst>(I) || isa<LoadInst>(I) || isa<CallInst>(I) ||
          isa<ReturnInst>(I) || isa<PHINode>(I) || isa<GetElementPtrInst>(I)) {
        path.push_back(V);
      }
    }
  }

  return path;
}

std::vector<const Value *> getWitnessPathGuided(
    const Value *From, const Value *To, DyckVFG *VFG,
    const std::unordered_map<const Value *, std::unordered_set<const Value *>>
        &AllReachabilityMap) {

  std::vector<const Value *> path;

  // Sanity checks
  if (!From || !To || !VFG) {
    return path;
  }

  // BFS to find a path, but only explore values reachable from source
  std::queue<const Value *> worklist;
  std::unordered_map<const Value *, const Value *> parent;
  std::unordered_set<const Value *> visited;

  worklist.push(From);
  visited.insert(From);
  parent[From] = nullptr;

  // Limit BFS to avoid infinite loops or very long searches
  const size_t MAX_ITERATIONS = 10000;
  size_t iterations = 0;

  bool found = false;
  while (!worklist.empty() && !found && iterations++ < MAX_ITERATIONS) {
    const Value *current = worklist.front();
    worklist.pop();

    if (current == To) {
      found = true;
      break;
    }

    // Get successors through VFG
    std::vector<const Value *> succs = getSuccessors(current, VFG);
    for (const Value *succ : succs) {
      if (!succ)
        continue; // Skip null successors

      // Only explore if this successor is reachable from our source
      // This is the key optimization when using detailed reachability info
      auto It = AllReachabilityMap.find(succ);
      if (It == AllReachabilityMap.end() || !It->second.count(From)) {
        continue; // Not reachable from source, skip it
      }

      if (visited.find(succ) == visited.end()) {
        visited.insert(succ);
        parent[succ] = current;
        worklist.push(succ);

        if (succ == To) {
          found = true;
          break;
        }
      }
    }
  }

  if (!found) {
    return path; // Empty path if not reachable
  }

  // Reconstruct path from parent pointers
  std::vector<const Value *> fullPath;
  const Value *current = To;
  while (current != nullptr) {
    fullPath.push_back(current);
    current = parent[current];
  }
  std::reverse(fullPath.begin(), fullPath.end());

  // Filter to keep only interesting propagation points
  for (const Value *V : fullPath) {
    if (V == From || V == To) {
      // Always include source and sink
      path.push_back(V);
    } else if (const Instruction *I = dyn_cast<Instruction>(V)) {
      if (isa<StoreInst>(I) || isa<LoadInst>(I) || isa<CallInst>(I) ||
          isa<ReturnInst>(I) || isa<PHINode>(I) || isa<GetElementPtrInst>(I)) {
        path.push_back(V);
      }
    }
  }

  // Limit path length to avoid overwhelming reports
  const size_t MAX_PATH_STEPS = 8;
  if (path.size() > MAX_PATH_STEPS) {
    std::vector<const Value *> truncated;
    // Keep first 3 (including source)
    for (size_t i = 0; i < 3 && i < path.size(); ++i) {
      truncated.push_back(path[i]);
    }
    // Add ellipsis marker (null pointer)
    truncated.push_back(nullptr);
    // Keep last 3 (including sink)
    size_t start = path.size() - 3;
    for (size_t i = start; i < path.size(); ++i) {
      truncated.push_back(path[i]);
    }
    path = truncated;
  }

  return path;
}

} // namespace GVFAUtils
