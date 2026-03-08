/*
FastGVFAEngine.cpp

Implementation of the Fast Global Value Flow Analysis Engine.
This engine optimizes reachability analysis by using bit-vectors (masks) for
forward propagation and simple counters for backward propagation.

Key Features:
- Uses integer bitmasks to track reachability from multiple sources
simultaneously.
- Significantly faster and lower memory usage than PreciseGVFAEngine.
- Trade-off: Lower precision.
  - srcReachable() returns false as individual source tracking is abstracted.
  - Backward analysis counts reachable sinks but doesn't identify WHICH sink is
reachable.

@Author: rainoftime
*/

#include "Analysis/GVFA/FastGVFAEngine.h"

#include "Analysis/GVFA/GVFAUtils.h"
#include "Utils/LLVM/RecursiveTimer.h"

#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace gvfa;

#define DEBUG_TYPE "dyck-gvfa"

void FastGVFAEngine::run() {
  RecursiveTimer Timer("DyckGVFA-Optimized");

  ReachabilityMap.clear();
  while (!SourcesVec.empty()) {
    LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzBeforeExtend: " << SourcesVec.size()
                      << "\n");
    extendSources(SourcesVec);
    LLVM_DEBUG(dbgs() << "[DEBUG] SrcVecSzAfterExtend: " << SourcesVec.size()
                      << "\n");
    // Run forward bit-vector analysis
    forwardRun();
  }

  LLVM_DEBUG({
    unsigned I = 0;
    for (auto &It : ReachabilityMap) {
      if (It.second)
        ++I;
    }
    dbgs() << "[DEBUG] ReachableNodesSz: " << I << "\n";
  });

  outs() << "[Opt-Indexing FW] Map Size: " << ReachabilityMap.size() << "\n";

  BackwardReachabilityMap.clear();
  // Run backward count-based analysis
  backwardRun();

  outs() << "[Opt-Indexing BW] Map Size: " << BackwardReachabilityMap.size()
         << "\n";
}

bool FastGVFAEngine::srcReachable(const Value *V,
                                  const ValueSitePairType &Src) const {
  // Fast mode does not track specific source reachability map (Value ->
  // Set<Src>). It only tracks (Value -> Mask). While we could check if the Mask
  // of Src is set in V's mask, this function expects precise object identity
  // which might be lost or aliased in the mask.
  return false;
}

std::vector<const Value *>
FastGVFAEngine::getWitnessPath(const Value *From, const Value *To) const {
  // Use unguided BFS since we don't have the precise 'AllReachabilityMap'
  // to guide the search like in PreciseGVFAEngine.
  return GVFAUtils::getWitnessPath(From, To, VFG);
}

void FastGVFAEngine::forwardRun() {
  // Move current batch of sources to local list to process
  auto SourceList = std::move(SourcesVec);
  // SourcesVec is now empty, ready for next iteration if extendSources adds
  // more

  for (const auto &Src : SourceList) {
    // Src.second is the ID/Mask assigned to this source
    forwardReachability(Src.first, Src.second);
  }
}

void FastGVFAEngine::backwardRun() {
  // Multi-sink backward traversal: mark values that can reach any sink.
  std::queue<const Value *> WorkQueue;
  std::unordered_set<const Value *> Visited;

  for (const auto &Sink : Sinks) {
    WorkQueue.push(Sink.first);
  }

  while (!WorkQueue.empty()) {
    const Value *CurrentValue = WorkQueue.front();
    WorkQueue.pop();

    if (!Visited.insert(CurrentValue).second)
      continue;

    // Mark as reaching at least one sink.
    BackwardReachabilityMap[CurrentValue] = 1;

    if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
      for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
        auto *Pred = It->first->getValue();
        if (Visited.find(Pred) == Visited.end()) {
          WorkQueue.push(Pred);
        }
      }
    }
  }
}

// Propagate reachability masks using a worklist algorithm
void FastGVFAEngine::forwardReachability(const Value *V, int Mask) {
  std::queue<std::pair<const Value *, int>> WorkQueue;
  std::unordered_set<const Value *> Visited;

  WorkQueue.emplace(V, Mask);

  while (!WorkQueue.empty()) {
    auto front = WorkQueue.front();
    const Value *CurrentValue = front.first;
    int CurrentMask = front.second;
    WorkQueue.pop();

    // Basic cycle prevention for this specific propagation path
    if (!Visited.insert(CurrentValue).second)
      continue;

    // OR the mask into the reachability map
    ReachabilityMap[CurrentValue] |= CurrentMask;

    // Handle special instructions (Calls, Returns) if needed
    if (auto *CI = dyn_cast<CallInst>(CurrentValue)) {
      processCallSite(CI, CurrentValue, CurrentMask, WorkQueue);
    } else if (auto *RI = dyn_cast<ReturnInst>(CurrentValue)) {
      processReturnSite(RI, CurrentValue, CurrentMask, WorkQueue);
    }

    // Propagate to VFG successors
    if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
      for (auto It = Node->begin(); It != Node->end(); ++It) {
        auto *Succ = It->first->getValue();
        // 'count' likely checks if Succ already has all bits in CurrentMask
        // set. It returns the 'UncoveredMask' (bits not yet set in Succ).
        if (int UncoveredMask = count(Succ, CurrentMask)) {
          if (Visited.find(Succ) == Visited.end()) {
            WorkQueue.emplace(Succ, UncoveredMask);
          }
        }
      }
    }
  }
}

// Backward reachability just counts/marks nodes that can reach a sink
void FastGVFAEngine::backwardReachability(const Value *V) {
  std::queue<const Value *> WorkQueue;
  std::unordered_set<const Value *> Visited;

  WorkQueue.push(V);

  while (!WorkQueue.empty()) {
    const Value *CurrentValue = WorkQueue.front();
    WorkQueue.pop();

    if (!Visited.insert(CurrentValue).second)
      continue;

    // Increment count of sinks reachable from CurrentValue
    BackwardReachabilityMap[CurrentValue] += 1;

    if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
      for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
        auto *Pred = It->first->getValue();
        // Propagate backward if not already visited/saturated
        if (!backwardCount(Pred) && Visited.find(Pred) == Visited.end()) {
          WorkQueue.push(Pred);
        }
      }
    }
  }
}
