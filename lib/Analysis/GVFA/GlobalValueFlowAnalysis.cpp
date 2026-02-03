/**
 * @file GlobalValueFlowAnalysis.cpp
 * @brief Global Value Flow Analysis using Dyck VFG
 *
 * Tracks data flow from vulnerability sources to sinks using a fast index for
 * pruning and lazy refinement for source attribution / witness traces, plus
 * context-sensitive CFL reachability.
 */

#include <llvm/IR/Argument.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <unordered_map>


#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Analysis/GVFA/GVFAUtils.h"
#include "Analysis/GVFA/HybridGVFAEngine.h"
#include "Checker/GVFA/GVFAVulnerabilityChecker.h"
#include "Utils/LLVM/RecursiveTimer.h"

using namespace llvm;
using namespace gvfa;

#define DEBUG_TYPE "dyck-gvfa"

// Command line options for analysis configuration
static cl::opt<bool> EnableOnlineQuery("enable-online-query",
                                       cl::desc("enable online query"),
                                       cl::init(false));

// Mutex for thread-safe online query timing
static std::mutex ClearMutex;

// Helper macro for online query timing
#define TIME_ONLINE_QUERY(expr) \
    if (EnableOnlineQuery.getValue()) { \
        auto start_time = std::chrono::high_resolution_clock::now(); \
        auto res = (expr); \
        auto end_time = std::chrono::high_resolution_clock::now(); \
        auto elapsed_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count(); \
        std::lock_guard<std::mutex> L(ClearMutex); \
        SnapshotedOnlineTime += elapsed_time; \
        if (res) SuccsQueryCounter++; \
        return res; \
    }

//===----------------------------------------------------------------------===//
// Implementation
//===----------------------------------------------------------------------===//

// Constructor

DyckGlobalValueFlowAnalysis::DyckGlobalValueFlowAnalysis(Module *M, DyckVFG *VFG, 
                                                         DyckAliasAnalysis *DyckAA, 
                                                         DyckModRefAnalysis *DyckMRA)
    : VFG(VFG), DyckAA(DyckAA), DyckMRA(DyckMRA), M(M) {}

DyckGlobalValueFlowAnalysis::~DyckGlobalValueFlowAnalysis() = default;

// Set vulnerability checker

void DyckGlobalValueFlowAnalysis::setVulnerabilityChecker(std::unique_ptr<GVFAVulnerabilityChecker> checker) {
    VulnChecker = std::move(checker);
}

// Print online query timing

void DyckGlobalValueFlowAnalysis::printOnlineQueryTime(llvm::raw_ostream &O, const char *Title) const {
    if (EnableOnlineQuery.getValue()) {
        long Ms = SnapshotedOnlineTime / 1000;
        O << Title << " Time: " << Ms / 1000 << "." << (Ms % 1000) / 100
          << (Ms % 1000) / 10 << "s\n";
    }
}

//===----------------------------------------------------------------------===//
// Main analysis entry point
//===----------------------------------------------------------------------===//

void DyckGlobalValueFlowAnalysis::run() {
    if (!VulnChecker) {
        errs() << "Warning: No vulnerability checker set\n";
        return;
    }
    
    // Get sources and sinks from vulnerability checker
    VulnChecker->getSources(M, Sources);
    VulnChecker->getSinks(M, Sinks);
    
    if (Sources.empty() || Sinks.empty()) {
        return;
    }
    
    // Convert sources to vector format
    for (auto &It : Sources) {
        auto *SrcValue = It.first.first;
        int Mask = It.second;
        SourcesVec.emplace_back(SrcValue, Mask);
    }
    
    outs() << "#Sources: " << SourcesVec.size() << "\n";
    outs() << "#Sinks: " << Sinks.size() << "\n";
    
    Engine = std::make_unique<HybridGVFAEngine>(M, VFG, DyckAA, DyckMRA,
                                                std::move(SourcesVec), Sinks);
    
    Engine->run();
}

//===----------------------------------------------------------------------===//
// Query interfaces
//===----------------------------------------------------------------------===//

int DyckGlobalValueFlowAnalysis::reachable(const Value *V, int Mask) {
    AllQueryCounter++;
    TIME_ONLINE_QUERY(onlineReachability(V) ? 1 : 0);
    
    if (Engine) {
        int res = Engine->reachable(V, Mask);
        if (res) SuccsQueryCounter++;
        return res;
    }
    return 0;
}

bool DyckGlobalValueFlowAnalysis::srcReachable(const Value *V, const Value *Src) const {
    if (Engine) {
        return Engine->srcReachable(V, Src);
    }
    return false;
}

bool DyckGlobalValueFlowAnalysis::backwardReachable(const Value *V) {
    if (Sinks.empty()) return true;
    AllQueryCounter++;
    TIME_ONLINE_QUERY(onlineReachability(V));
    
    if (Engine) {
        bool res = Engine->backwardReachable(V);
        if (res) SuccsQueryCounter++;
        return res;
    }
    return false;
}

bool DyckGlobalValueFlowAnalysis::backwardReachableSink(const Value *V) {
    AllQueryCounter++;
    TIME_ONLINE_QUERY(onlineReachability(V));
    
    if (Engine) {
        bool res = Engine->backwardReachableSink(V);
        if (res) SuccsQueryCounter++;
        return res;
    }
    return false;
}

bool DyckGlobalValueFlowAnalysis::backwardReachableAllSinks(const Value *V) {
    if (Engine) {
        return Engine->backwardReachableAllSinks(V);
    }
    return false;
}

std::vector<const Value *> DyckGlobalValueFlowAnalysis::getWitnessPath(
    const Value *From, const Value *To) const {
    if (Engine) {
        return Engine->getWitnessPath(From, To);
    }
    return {};
}

//===----------------------------------------------------------------------===//
// Online queries (Copied from ReachabilityAlgorithms.cpp)
//===----------------------------------------------------------------------===//

bool DyckGlobalValueFlowAnalysis::onlineReachability(const Value *Target) {
    for (const auto &Sink : Sinks) {
        std::unordered_set<const Value *> visited;
        if (onlineBackwardReachability(Sink.first, Target, visited)) return true;
    }
    return false;
}

bool DyckGlobalValueFlowAnalysis::onlineForwardReachability(const Value *V, 
                                                      std::unordered_set<const Value *> &visited) {
    std::queue<const Value *> WorkQueue;
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (!visited.insert(CurrentValue).second) continue;
        
        if (Sinks.count(CurrentValue)) return true;
        
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->begin(); It != Node->end(); ++It) {
                auto *Succ = It->first->getValue();
                if (visited.find(Succ) == visited.end()) {
                    WorkQueue.push(Succ);
                }
            }
        }
    }
    
    return false;
}

bool DyckGlobalValueFlowAnalysis::onlineBackwardReachability(const Value *V, const Value *Target,
                                                       std::unordered_set<const Value *> &visited) {
    std::queue<const Value *> WorkQueue;
    WorkQueue.push(V);
    
    while (!WorkQueue.empty()) {
        const Value *CurrentValue = WorkQueue.front();
        WorkQueue.pop();
        
        if (!visited.insert(CurrentValue).second) continue;
        
        if (CurrentValue == Target) return true;
        
        if (auto *Node = VFG->getVFGNode(const_cast<Value *>(CurrentValue))) {
            for (auto It = Node->in_begin(); It != Node->in_end(); ++It) {
                auto *Pred = It->first->getValue();
                if (visited.find(Pred) == visited.end()) {
                    WorkQueue.push(Pred);
                }
            }
        }
    }
    
    return false;
}

//===----------------------------------------------------------------------===//
// CFL Reachability (Copied from ReachabilityAlgorithms.cpp)
//===----------------------------------------------------------------------===//

void DyckGlobalValueFlowAnalysis::initializeCFLAnalyzer() {
    // Lightweight CFL analyzer works directly with VFG's existing label structure
}

bool DyckGlobalValueFlowAnalysis::cflReachable(const Value *From, const Value *To) const {
    return cflReachabilityQuery(From, To, true);
}

bool DyckGlobalValueFlowAnalysis::cflBackwardReachable(const Value *From, const Value *To) const {
    return cflReachabilityQuery(To, From, false);
}

bool DyckGlobalValueFlowAnalysis::contextSensitiveReachable(const Value *From, const Value *To) const {
    return cflReachable(From, To);
}

bool DyckGlobalValueFlowAnalysis::contextSensitiveBackwardReachable(const Value *From, const Value *To) const {
    return cflBackwardReachable(From, To);
}

bool DyckGlobalValueFlowAnalysis::performCFLReachabilityQuery(const Value *From, const Value *To, bool Forward) const {
    return cflReachabilityQuery(From, To, Forward);
}

bool DyckGlobalValueFlowAnalysis::cflReachabilityQuery(const Value *From, const Value *To, bool Forward) const {
    std::unordered_set<const Value *> visited;
    std::queue<std::pair<const Value *, std::vector<int>>> workQueue;
    
    workQueue.push(std::make_pair(From, std::vector<int>()));
    
    while (!workQueue.empty()) {
        auto front = workQueue.front();
        const Value *current = front.first;
        std::vector<int> callStack = front.second;
        workQueue.pop();
        
        if (current == To) return true;
        if (!visited.insert(current).second) continue;
        
        auto *currentNode = VFG->getVFGNode(const_cast<Value *>(current));
        if (!currentNode) continue;
        
        if (Forward) {
            for (auto edgeIt = currentNode->begin(); edgeIt != currentNode->end(); ++edgeIt) {
                auto *nextValue = edgeIt->first->getValue();
                int label = edgeIt->second;
                
                std::vector<int> newCallStack = callStack;
                
                if (label > 0) {
                    newCallStack.push_back(label);
                } else if (label < 0) {
                    if (newCallStack.empty() || newCallStack.back() != -label) continue;
                    newCallStack.pop_back();
                }
                
                if (visited.find(nextValue) == visited.end()) {
                    workQueue.push(std::make_pair(nextValue, std::move(newCallStack)));
                }
            }
        } else {
            for (auto edgeIt = currentNode->in_begin(); edgeIt != currentNode->in_end(); ++edgeIt) {
                auto *prevValue = edgeIt->first->getValue();
                int label = edgeIt->second;
                
                std::vector<int> newCallStack = callStack;
                
                if (label < 0) {
                    newCallStack.push_back(-label);
                } else if (label > 0) {
                    if (newCallStack.empty() || newCallStack.back() != label) continue;
                    newCallStack.pop_back();
                }
                
                if (visited.find(prevValue) == visited.end()) {
                    workQueue.push(std::make_pair(prevValue, std::move(newCallStack)));
                }
            }
        }
    }
    
    return false;
}

int DyckGlobalValueFlowAnalysis::getValueNodeID(const Value *V) const {
    return reinterpret_cast<intptr_t>(V);
}
