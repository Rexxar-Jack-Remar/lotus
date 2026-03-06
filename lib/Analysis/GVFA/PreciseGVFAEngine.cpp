/*
PreciseGVFAEngine.cpp

Implementation of the Precise Global Value Flow Analysis Engine.
This engine performs a full reachability analysis tracking the exact set of
sources and sinks reachable from each value in the VFG.

Key Features:
- Uses std::set (via unordered_map to set) to track reachability.
- Supports precise 'srcReachable' queries: can determine if a specific source V
reaches a value.
- Supports guided witness path generation using the computed reachability sets.
- Memory intensive due to storing sets of pointers for each reachable node.

@Author: rainoftime
*/

#include "Analysis/GVFA/PreciseGVFAEngine.h"

#include "Analysis/GVFA/GVFAUtils.h"
#include "Utils/LLVM/RecursiveTimer.h"

#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace gvfa;

#define DEBUG_TYPE "dyck-gvfa"

void PreciseGVFAEngine::run() {
  RecursiveTimer Timer("DyckGVFA-Detailed");

  // Clear previous results
  AllReachabilityMap.clear();

  // Process sources iteratively as they are discovered/extended
  while (!SourcesVec.empty()) {
    LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzBeforeExtend: " << SourcesVec.size()
                      << "\n");
    // Expand sources (e.g. handle aliases, globals)
    extendSources(SourcesVec);
    LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzAfterExtend: " << SourcesVec.size()
                      << "\n");

    // Run forward analysis for the current batch of sources
    detailedForwardRun();
  }

  outs() << "[Indexing FW] Map Size: " << AllReachabilityMap.size() << "\n";

  AllBackwardReachabilityMap.clear();
  // Run backward analysis from all sinks
  detailedBackwardRun();

  outs() << "[Indexing BW] Map Size: " << AllBackwardReachabilityMap.size()
         << "\n";
}

// Check reachability using the precise set-based map
int PreciseGVFAEngine::reachable(const Value *V, int Mask) {
  int ResultMask = 0;
  auto It = AllReachabilityMap.find(V);
  if (It != AllReachabilityMap.end()) {
    // Check if any of the reachable sources overlap with the requested Mask
    // Note: This assumes sources in ReachabilityMap (from base class) map to
    // the Mask bits
    for (const Value *Src : It->second) {
      auto SrcIt = ReachabilityMap.find(Src);
      if (SrcIt != ReachabilityMap.end()) {
        ResultMask |= (SrcIt->second & Mask);
      }
    }
  }
  return ResultMask;
}

// Precise check: Is specific Src in the reachability set of V?
bool PreciseGVFAEngine::srcReachable(const Value *V, const Value *Src) const {
  auto It = AllReachabilityMap.find(V);
  return (It != AllReachabilityMap.end()) && It->second.count(Src);
}

bool PreciseGVFAEngine::backwardReachableSink(const Value *V) {
  auto It = AllBackwardReachabilityMap.find(V);
  if (It != AllBackwardReachabilityMap.end() && !It->second.empty()) {
    return true;
  }
  return false;
}

bool PreciseGVFAEngine::backwardReachableAllSinks(const Value *V) {
  auto It = AllBackwardReachabilityMap.find(V);
  if (It == AllBackwardReachabilityMap.end()) {
    return false;
  }
  // Check if the number of reachable sinks equals the total number of sinks
  if (It->second.size() != Sinks.size()) {
    return false;
  }
  return true;
}

std::vector<const Value *>
PreciseGVFAEngine::getWitnessPath(const Value *From, const Value *To) const {
  // If we have reachability info, use it to guide the BFS/DFS to find the path
  // faster
  if (!AllReachabilityMap.empty()) {
    auto It = AllReachabilityMap.find(To);
    if (It == AllReachabilityMap.end() || !It->second.count(From)) {
      return {}; // Early exit if we know there is no path
    }
    return GVFAUtils::getWitnessPathGuided(From, To, VFG, AllReachabilityMap);
  }
  return GVFAUtils::getWitnessPath(From, To, VFG);
}

void PreciseGVFAEngine::detailedForwardRun() {
  auto SourceList = std::move(SourcesVec);
  // For every individual source, run a traversal to mark everything it reaches
  for (const auto &Src : SourceList) {
    detailedForwardReachability(Src.first, Src.first);
  }
}

void PreciseGVFAEngine::detailedBackwardRun() {
  // For every individual sink, run a backward traversal
  for (const auto &Sink : Sinks) {
    detailedBackwardReachability(Sink.first, Sink.first);
  }
}

// BFS to propagate reachability of 'Src' to all reachable nodes
void PreciseGVFAEngine::detailedForwardReachability(const Value *V,
                                                    const Value *Src) {
  std::queue<const Value *> WorkQueue;
  std::unordered_set<const Value *> Visited;

  WorkQueue.push(V);

  while (!WorkQueue.empty()) {
    const Value *CurrentValue = WorkQueue.front();
    WorkQueue.pop();

    // Prevent cycles/redundant work for this specific Source propagation
    if (!Visited.insert(CurrentValue).second)
      continue;

    // Add Src to the set of sources reaching CurrentValue
    AllReachabilityMap[CurrentValue].insert(Src);

    if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
      for (auto It = Node->begin(); It != Node->end(); ++It) {
        auto *Succ = It->first->getValue();
        // Continue if Succ hasn't already been marked as reached by Src
        if (!allCount(Succ, Src) && Visited.find(Succ) == Visited.end()) {
          WorkQueue.push(Succ);
        }
      }
    }
  }
}

// BFS to propagate backward reachability of 'Sink'
void PreciseGVFAEngine::detailedBackwardReachability(const Value *V,
                                                     const Value *Sink) {
  std::queue<const Value *> WorkQueue;
  std::unordered_set<const Value *> Visited;

  WorkQueue.push(V);

  while (!WorkQueue.empty()) {
    const Value *CurrentValue = WorkQueue.front();
    WorkQueue.pop();

    if (!Visited.insert(CurrentValue).second)
      continue;

    // Add Sink to the set of sinks reachable from CurrentValue
    AllBackwardReachabilityMap[CurrentValue].insert(Sink);

    if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
      for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
        auto *Pred = It->first->getValue();
        // Continue if Pred hasn't already been marked as reaching Sink
        if (!allBackwardCount(Pred, Sink) &&
            Visited.find(Pred) == Visited.end()) {
          WorkQueue.push(Pred);
        }
      }
    }
  }
}

// Helper: check if V is already marked as reached by Src
bool PreciseGVFAEngine::allCount(const Value *V, const Value *Src) {
  auto &Set = AllReachabilityMap[V];
  if (Set.count(Src)) {
    return true;
  } else {
    Set.insert(Src);
    return false;
  }
}

// Helper: check if V is already marked as reaching Sink
bool PreciseGVFAEngine::allBackwardCount(const Value *V, const Value *Sink) {
  auto &Set = AllBackwardReachabilityMap[V];
  if (Set.count(Sink)) {
    return true;
  } else {
    Set.insert(Sink);
    return false;
  }
}
