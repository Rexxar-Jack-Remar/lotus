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

PreciseGVFAEngine::PreciseGVFAEngine(
    Module *M, DyckVFG *VFG, DyckAliasAnalysis *DyckAA,
    DyckModRefAnalysis *DyckMRA,
    std::vector<std::pair<const Value *, int>> SourcesVec,
    std::vector<ValueSitePairType> SourceSitesVec,
    const VulnerabilitySinksType &Sinks)
    : GVFAEngine(M, VFG, DyckAA, DyckMRA, std::move(SourcesVec), Sinks),
      OriginalSourceSites(std::move(SourceSitesVec)) {
  for (size_t I = 0; I < OriginalSourceSites.size() && I < this->SourcesVec.size();
       ++I) {
    SourceSiteMasks[OriginalSourceSites[I].second] = this->SourcesVec[I].second;
  }
}

void PreciseGVFAEngine::run() {
  RecursiveTimer Timer("DyckGVFA-Detailed");

  // Clear previous results
  AllReachabilityMap.clear();

  auto ExpandedSourceSites = OriginalSourceSites;
  LLVM_DEBUG(dbgs() << "[DEBUG] SrcSiteSzBeforeExtend: "
                    << ExpandedSourceSites.size() << "\n");
  extendSourceSites(ExpandedSourceSites);
  LLVM_DEBUG(dbgs() << "[DEBUG] SrcSiteSzAfterExtend: "
                    << ExpandedSourceSites.size() << "\n");

  // Run forward analysis for all expanded source sites
  detailedForwardRun(ExpandedSourceSites);

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
    for (const ValueSitePairType &Src : It->second) {
      auto SrcIt = SourceSiteMasks.find(Src.second);
      if (SrcIt != SourceSiteMasks.end()) {
        ResultMask |= (SrcIt->second & Mask);
      }
    }
  }
  return ResultMask;
}

// Precise check: Is specific Src in the reachability set of V?
bool PreciseGVFAEngine::srcReachable(const Value *V,
                                     const ValueSitePairType &Src) const {
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
  return GVFAUtils::getWitnessPath(From, To, VFG);
}

void PreciseGVFAEngine::extendSourceSites(
    std::vector<ValueSitePairType> &SourceSites) {
  std::queue<ValueSitePairType> WorkQueue;
  std::unordered_set<ValueSitePairType, SourceSiteHash> Visited;
  std::unordered_map<const Value *, std::vector<const Value *>> PredCache;

  auto getPredecessors = [&PredCache, this](const Value *V)
      -> const std::vector<const Value *> & {
    auto It = PredCache.find(V);
    if (It != PredCache.end()) {
      return It->second;
    }

    std::vector<const Value *> Preds;
    if (auto *Arg = dyn_cast<Argument>(V)) {
      const Function *F = Arg->getParent();
      unsigned ArgIdx = Arg->getArgNo();
      for (auto *User : F->users()) {
        if (auto *CI = dyn_cast<CallInst>(User)) {
          if (CI->getCalledFunction() == F && ArgIdx < CI->arg_size()) {
            Preds.push_back(CI->getArgOperand(ArgIdx));
          }
        }
      }
    } else if (auto *CI = dyn_cast<CallInst>(V)) {
      if (auto *F = CI->getCalledFunction()) {
        for (auto &BB : *F) {
          for (auto &I : BB) {
            if (auto *RI = dyn_cast<ReturnInst>(&I)) {
              if (RI->getReturnValue()) {
                Preds.push_back(RI->getReturnValue());
              }
            }
          }
        }
      }
    } else if (auto *Node = VFG->getVFGNode(const_cast<Value *>(V))) {
      for (auto ItPred = Node->in_begin(); ItPred != Node->in_end(); ++ItPred) {
        Preds.push_back(ItPred->first->getValue());
      }
    }

    auto Inserted = PredCache.emplace(V, std::move(Preds));
    return Inserted.first->second;
  };

  for (const ValueSitePairType &Site : SourceSites) {
    WorkQueue.push(Site);
  }

  SourceSites.clear();

  while (!WorkQueue.empty()) {
    ValueSitePairType CurrentSite = WorkQueue.front();
    WorkQueue.pop();

    if (!Visited.insert(CurrentSite).second) {
      continue;
    }

    SourceSites.push_back(CurrentSite);
    const auto &Preds = getPredecessors(CurrentSite.first);
    for (const Value *Pred : Preds) {
      WorkQueue.emplace(Pred, CurrentSite.second);
    }
  }
}

void PreciseGVFAEngine::detailedForwardRun(
    const std::vector<ValueSitePairType> &SourceSites) {
  for (const ValueSitePairType &Src : SourceSites) {
    detailedForwardReachability(Src.first, Src);
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
                                                    const ValueSitePairType &Src) {
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
bool PreciseGVFAEngine::allCount(const Value *V,
                                 const ValueSitePairType &Src) {
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
