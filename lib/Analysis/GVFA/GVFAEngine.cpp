#include "Analysis/GVFA/GVFAEngine.h"
#include "Analysis/GVFA/GVFAUtils.h"
#include <llvm/IR/Argument.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace gvfa;

GVFAEngine::GVFAEngine(Module *M, DyckVFG *VFG, DyckAliasAnalysis *DyckAA, 
                       DyckModRefAnalysis *DyckMRA,
                       std::vector<std::pair<const Value *, int>> SourcesVec,
                       const VulnerabilitySinksType &Sinks)
    : M(M), VFG(VFG), DyckAA(DyckAA), DyckMRA(DyckMRA), 
      SourcesVec(std::move(SourcesVec)), Sinks(Sinks) {}

// Base implementation uses ReachabilityMap (BitVector)
int GVFAEngine::reachable(const Value *V, int Mask) {
    auto It = ReachabilityMap.find(V);
    int UncoveredMask = (It != ReachabilityMap.end()) ? (Mask & ~(Mask & It->second)) : Mask;
    return Mask & ~UncoveredMask;
}

bool GVFAEngine::backwardReachable(const Value *V) {
    auto It = BackwardReachabilityMap.find(V);
    if (It == BackwardReachabilityMap.end() || It->second == 0) {
        return false;
    }
    return true;
}

bool GVFAEngine::backwardReachableSink(const Value *V) {
    // In Fast mode, we don't distinguish sinks in backward map usually?
    // The original code used AllBackwardReachabilityMap for this check in detailed mode?
    // Wait, optimizedBackwardRun fills BackwardReachabilityMap.
    // But backwardReachableSink in original code (line 189) used AllBackwardReachabilityMap.
    // Ah, the original code had:
    // backwardReachable -> Checks BackwardReachabilityMap
    // backwardReachableSink -> Checks AllBackwardReachabilityMap
    // This implies Fast mode CANNOT answer backwardReachableSink correctly if it only has bitmasks?
    // Or does BackwardReachabilityMap store Sink IDs?
    // Line 117: BackwardReachabilityMap[V] = 1; -> It just counts *visits*, not masks.
    // So Fast mode just knows "it reaches SOME sink".
    // So backwardReachableSink should return true if backwardReachable returns true?
    return backwardReachable(V);
}

bool GVFAEngine::backwardReachableAllSinks(const Value *V) {
    // Fast mode cannot support this accurately with single bit
    return false;
}

//===----------------------------------------------------------------------===//
// Helper Algorithms
//===----------------------------------------------------------------------===//

int GVFAEngine::count(const Value *V, int Mask) {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return Mask & ~(Mask & It->second);
    } else {
        ReachabilityMap[V] = 0;
        return Mask;
    }
}

bool GVFAEngine::count(const Value *V) {
    auto It = ReachabilityMap.find(V);
    if (It != ReachabilityMap.end()) {
        return true;
    } else {
        ReachabilityMap[V] = 1;
        return false;
    }
}

int GVFAEngine::backwardCount(const Value *V) {
    auto It = BackwardReachabilityMap.find(V);
    if (It != BackwardReachabilityMap.end()) {
        return It->second;
    } else {
        BackwardReachabilityMap[V] = 1;
        return 0;
    }
}

void GVFAEngine::extendSources(std::vector<std::pair<const Value *, int>> &Sources) {
    std::unordered_map<const Value *, int> NewSrcMap;
    std::queue<std::pair<const Value *, int>> WorkQueue;
    std::unordered_map<const Value *, std::vector<const Value *>> PredCache;
    
    // Initialize work queue with current sources
    for (const auto &Source : Sources) {
        WorkQueue.push(Source);
        NewSrcMap[Source.first] = Source.second;
    }
    
    Sources.clear();
    
    auto count_lambda = [&NewSrcMap, this](const Value *V, int Mask) -> int {
        int Uncovered = 0;
        
        auto It = NewSrcMap.find(V);
        if (It != NewSrcMap.end()) {
            Uncovered = Mask & ~(Mask & It->second);
        } else {
            NewSrcMap[V] = 0;
            Uncovered = Mask;
        }
        return this->count(V, Uncovered);
    };
    
    auto getPredecessors = [&PredCache, this](const Value *V) -> const std::vector<const Value *> & {
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
        } else {
            if (auto *Node = VFG->getVFGNode(const_cast<Value *>(V))) {
                for (auto ItPred = Node->in_begin(); ItPred != Node->in_end(); ++ItPred) {
                    Preds.push_back(ItPred->first->getValue());
                }
            }
        }

        auto Inserted = PredCache.emplace(V, std::move(Preds));
        return Inserted.first->second;
    };

    // Process work queue iteratively
    while (!WorkQueue.empty()) {
        auto front = WorkQueue.front();
        const Value *CurrentValue = front.first;
        int CurrentMask = front.second;
        WorkQueue.pop();
        
        if (count_lambda(CurrentValue, CurrentMask) == 0) {
            continue;
        }
        
        NewSrcMap[CurrentValue] |= CurrentMask;
        
        const auto &Preds = getPredecessors(CurrentValue);
        for (const auto *Pred : Preds) {
            if (int UncoveredMask = count_lambda(Pred, CurrentMask)) {
                WorkQueue.emplace(Pred, UncoveredMask);
            }
        }
    }
    
    for (auto &It : NewSrcMap) {
        if (It.second) {
            Sources.emplace_back(It.first, It.second);
        }
    }
}

void GVFAEngine::processCallSite(const CallInst *CI, const Value *V, int Mask, 
                               std::queue<std::pair<const Value *, int>> &WorkQueue) {
    if (auto *F = CI->getCalledFunction()) {
        for (unsigned i = 0; i < CI->arg_size() && i < F->arg_size(); ++i) {
            if (CI->getArgOperand(i) == V) {
                if (int UncoveredMask = count(F->getArg(i), Mask)) {
                    WorkQueue.emplace(F->getArg(i), UncoveredMask);
                }
            }
        }
    }
}

void GVFAEngine::processReturnSite(const ReturnInst *RI, const Value *V, int Mask,
                                 std::queue<std::pair<const Value *, int>> &WorkQueue) {
    const Function *F = RI->getFunction();
    
    for (auto *User : F->users()) {
        if (auto *CI = dyn_cast<CallInst>(User)) {
            if (CI->getCalledFunction() == F && RI->getReturnValue() == V) {
                if (int UncoveredMask = count(CI, Mask)) {
                    WorkQueue.emplace(CI, UncoveredMask);
                }
            }
        }
    }
}
