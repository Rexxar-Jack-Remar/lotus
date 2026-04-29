/*
 * LotusAA - Module-Level Alias Analysis Pass
 * 
 * Top-level LLVM pass that orchestrates pointer analysis across the entire module.
 * 
 * Key Responsibilities:
 * - Schedule bottom-up inter-procedural analysis
 * - Manage function-level analysis results
 * - Resolve indirect function calls using points-to information
 * - Provide query interface for alias analysis results
 */

#pragma once

#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "Alias/InclusionBased/LotusAA/MemoryModel/Types.h"
#include "Alias/InclusionBased/LotusAA/Support/CallGraphState.h"
#include "Alias/InclusionBased/LotusAA/Support/Compat.h"
#include "Alias/InclusionBased/LotusAA/Support/FunctionPointerResults.h"
#include "Alias/Infrastructure/Spec/AliasSpecManager.h"
#include "IR/GSA/GSA.h"

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace llvm {

class IntraLotusAA;
class PTGraph;

// Type aliases for improved readability
using AnalysisResultsMap = std::map<Function *, IntraLotusAA *, llvm_cmp>;
using GlobalValueCache = std::map<Value *, std::set<Value *, llvm_cmp>, llvm_cmp>;
using FunctionGroup = std::vector<Function *>;
using FunctionWave = std::vector<FunctionGroup>;
using FunctionWaveList = std::vector<FunctionWave>;

/*
 * LotusAA - Top-level pass for Lotus Alias Analysis
 * 
 * Schedules intra-procedural and inter-procedural analysis bottom-up
 */
class LotusAA : public ModulePass {
public:
  static char ID;

  LotusAA();
  virtual ~LotusAA();

  static void setParallelThreadsForTesting(unsigned thread_count);
  static void clearParallelThreadsForTesting();
  static void setFixedCallGraphModeForTesting(bool enabled);
  static void clearFixedCallGraphModeForTesting();

  void getAnalysisUsage(AnalysisUsage &) const override;
  bool runOnModule(Module &) override;

  // Compute PTA for a function (return true if interface changed)
  bool computePTA(Function *F);

  // Get intra-procedural analysis result
  IntraLotusAA *getPtGraph(Function *F);

  // Check if call is a back-edge
  bool isBackEdge(Function *caller, Function *callee);

  // Get possible callees for indirect call, keyed by target function.
  CallTargetSet *getCallees(Function *func, Value *callsite);

public:
  // Accessors for dependent analyses
  DominatorTree *getDomTree(Function *F);
  const DataLayout &getDataLayout() { return *DL; }

  // Access to call graph state
  CallGraphState &getCallGraphState() { return callGraphState_; }
  FunctionPointerResults &getFunctionPointerResults() { return functionPointerResults_; }
  
  // Access to spec manager
  lotus::alias::AliasSpecManager &getSpecManager() { return specManager_; }
  gsa::ControlDependenceAnalysis *getControlDependenceAnalysis(Function *F);

private:
  // Data layout
  const DataLayout *DL;

  // Intra-procedural analysis results
  AnalysisResultsMap intraResults_;

  // Staged results visible only during sequential SCC evaluation in the
  // fixed-callgraph scheduler.
  AnalysisResultsMap stagedResults_;
  bool stagedResultsVisible_ = false;

  // Call graph state (caller-callee relationships, back edges)
  CallGraphState callGraphState_;

  // Function pointer resolution results (indirect call targets)
  FunctionPointerResults functionPointerResults_;

  // Global value cache (for initialization heuristics)
  GlobalValueCache globalValuesCache_;

  // Cached dominator trees for each function
  std::map<Function *, DominatorTree *, llvm_cmp> dominatorTrees_;

  // Scheduler telemetry for tests.
  FunctionWaveList analysisWaves_;
  std::vector<unsigned> parallelSingletonCounts_;

  // Guards shared structures accessed from worker threads
  std::mutex domMutex_;
  
  // Spec manager for handling library functions
  lotus::alias::AliasSpecManager specManager_;

  friend class PTGraph;
  friend class IntraLotusAA;

private:
  static unsigned testingParallelThreadsOverride_;
  static int testingFixedCallGraphModeOverride_;

  void initFuncProcessingSeq(Module &M, std::vector<Function *> &func_seq);
  void initCGBackedge();
  void computeGlobalHeuristic(Module &M);
  void computePtsCgIteratively(Module &M, std::vector<Function *> &func_seq);
  void computePtsWithFixedCallGraph(Module &M, std::vector<Function *> &func_seq,
                                    unsigned requested_threads);
  void finalizeCg(std::vector<Function *> &func_seq);
};

} // namespace llvm
