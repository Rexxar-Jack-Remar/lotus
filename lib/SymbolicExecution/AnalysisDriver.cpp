//===----------------------------------------------------------------------===//
//
// AnalysisDriver orchestrates whole-module symbolic execution. It decides which
// functions to run, in what order summaries become available, and how per-
// function results are merged back into the module-level bug-trace store.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/AnalysisDriver.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Module.h"

#include "SymbolicExecution/GVFGUtility.h"
#include "SymbolicExecution/MemoryAPI.h"
#include "Utils/Parallel/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>

using namespace SymbolicExecution;

namespace {

static void topsortCFG(std::vector<BasicBlock *> &sorted, Function *F) {
  // Symbolic execution is still path sensitive, but visiting blocks in an
  // acyclic predecessor-first order reduces avoidable reprocessing for the
  // common forward-only cases. Backedges are appended afterwards.
  if (!F)
    return;

  std::unordered_map<BasicBlock *, unsigned> indegree;
  std::deque<BasicBlock *> worklist;

  for (BasicBlock &BB : *F) {
    unsigned count = 0;
    for (auto PI = pred_begin(&BB), PE = pred_end(&BB); PI != PE; ++PI)
      ++count;
    indegree[&BB] = count;
  }

  for (BasicBlock &BB : *F) {
    if (indegree[&BB] == 0)
      worklist.push_back(&BB);
  }

  while (!worklist.empty()) {
    BasicBlock *BB = worklist.front();
    worklist.pop_front();
    sorted.push_back(BB);

    for (BasicBlock *Succ : successors(BB)) {
      auto it = indegree.find(Succ);
      if (it == indegree.end())
        continue;
      if (it->second > 0)
        --it->second;
      if (it->second == 0)
        worklist.push_back(Succ);
    }
  }

  for (BasicBlock &BB : *F) {
    if (std::find(sorted.begin(), sorted.end(), &BB) == sorted.end())
      sorted.push_back(&BB);
  }
}

} // namespace

static unsigned ConfiguredBugTypes = AnalysisState::BUG_TY_BOF;

AnalysisDriver::AnalysisDriver() { initBugType(); }

void AnalysisDriver::setEnabledBugTypes(unsigned bug_types) {
  ConfiguredBugTypes = bug_types;
}

void AnalysisDriver::initBugType() {
  BugTy = static_cast<AnalysisState::SymexBugType>(ConfiguredBugTypes);
}

void AnalysisDriver::runOnModuleParallel(Module *M) {
  AnalysisState::NON_PTR_TY = Type::getInt64Ty(M->getContext());
  AnalysisState::INT8_TY = Type::getInt8Ty(M->getContext());
  SummarySolverManager::get().init();

  // getFuncSeq returns a summary-friendly order over the call graph. We walk it
  // in reverse so callees tend to finish before callers ask for their summary.
  gvfg_utility::getTopoOrder(*M);
  const auto &FuncSeq = gvfg_utility::getFuncSeq();
  assert(!FuncSeq.empty());

  std::vector<GuardedValueFlowGraph *> Worklist;
  Worklist.reserve(FuncSeq.size());

  for (auto Iter = FuncSeq.rbegin(), EIter = FuncSeq.rend(); Iter != EIter;
       ++Iter) {
    Function *CurFunc = *Iter;
    if (CurFunc->isDeclaration()) {
      continue;
    }

    auto *Graph = gvfg_utility::getGraph(CurFunc);
    if (!Graph) {
      continue;
    }

    unsigned FunDepth = gvfg_utility::getFunctionDepth(CurFunc);
    if (FunDepth > AnalysisLimit::FUNC_INLINE_LIMIT_V) {
      llvm::errs() << "Skip function " << CurFunc->getName()
                   << " due to inline threshold!\n";
      continue;
    }

    Worklist.push_back(Graph);
  }

  std::atomic<unsigned> NumRemain(Worklist.size());
  std::mutex ProgressMtx;

  llvm::errs() << "[Progress] Start ... " << NumRemain.load()
               << "functions to run!\n";

  ThreadPool::get()->parallelForEach(
      Worklist, 1,
      [this, &NumRemain, &ProgressMtx](GuardedValueFlowGraph *Graph) {
        Function *CurFunc = Graph->getBaseFunction();
        unsigned FunDepth = gvfg_utility::getFunctionDepth(CurFunc);

        {
          std::lock_guard<std::mutex> Lock(ProgressMtx);
          llvm::errs() << "Running on " << CurFunc->getName()
                       << "(depth=" << FunDepth << ")"
                       << "\n";
        }

        runOnFunction(Graph);

        unsigned Remain = NumRemain.fetch_sub(1) - 1;
        {
          std::lock_guard<std::mutex> Lock(ProgressMtx);
          llvm::errs() << "[Progress] " << Remain << "functions remains!\n";
        }
      });
}

void AnalysisDriver::runOnModule(Module *M) {
  AnalysisState::NON_PTR_TY = Type::getInt64Ty(M->getContext());
  AnalysisState::INT8_TY = Type::getInt8Ty(M->getContext());
  SummarySolverManager::get().init();

  // The sequential path uses the same scheduling policy as the parallel one so
  // summary availability and debugging behavior stay comparable.
  gvfg_utility::getTopoOrder(*M);
  const auto &FuncSeq = gvfg_utility::getFuncSeq();
  assert(!FuncSeq.empty());

  // The heartbleed bug: "ssl3_read_bytes"
  // Function *TargerFunc = M->getFunction("ssl3_read_bytes");
  // assert(TargerFunc);
  // auto TargetSlice = gvfg_utility::getCallSlice(TargerFunc);

  unsigned NumRemain = std::accumulate(FuncSeq.begin(), FuncSeq.end(), 0,
                                       [](unsigned Sum, Function *Fun) {
                                         if (Fun->isDeclaration()) {
                                           return Sum;
                                         } else {
                                           return Sum + 1;
                                         }
                                       });

  llvm::errs() << "[Progress] Start ... " << NumRemain << "functions to run!\n";
  for (auto Iter = FuncSeq.rbegin(), EIter = FuncSeq.rend(); Iter != EIter;
       ++Iter) {
    Function *CurFunc = *Iter;
    auto *Graph = gvfg_utility::getGraph(CurFunc);

    if (!Graph) {
      continue;
    }

    unsigned FunDepth = gvfg_utility::getFunctionDepth(CurFunc);
    if (FunDepth > AnalysisLimit::FUNC_INLINE_LIMIT_V) {
      llvm::errs() << "Skip function " << CurFunc->getName()
                   << " due to inline threshold!\n";
    } /*else if (!TargetSlice.count(CurFunc)) {
        llvm::errs() << "Cur function " << CurFunc->getName() << " not in the
    slice!\n";
    }*/
    else {
      llvm::errs() << "Running on " << CurFunc->getName()
                   << "(depth=" << FunDepth << ")"
                   << "\n";
      runOnFunction(Graph);
    }

    llvm::errs() << "[Progress] " << --NumRemain << "functions remains!\n";
  }
}

std::vector<std::tuple<AnalysisState::SymexBugType, std::vector<TaintStep>,
                       std::vector<TraceStep>>>
AnalysisDriver::getBugTraces() const {
  return Traces;
}

void AnalysisDriver::addSummary(Function *F, const AnalysisSummary &Smry) {
  std::lock_guard<std::mutex> Lock(AnalysisMtx);
  AnalysisRes.emplace(F, std::make_unique<AnalysisSummary>(Smry));
}

bool AnalysisDriver::hasSummary(Function *F) const {
  std::lock_guard<std::mutex> Lock(AnalysisMtx);
  return AnalysisRes.count(F);
}

const AnalysisSummary &AnalysisDriver::getSummary(Function *F) const {
  std::lock_guard<std::mutex> Lock(AnalysisMtx);
  return *AnalysisRes.at(F);
}

void AnalysisDriver::releaseMemForFunction(Function *F) {
  std::lock_guard<std::mutex> Lock1(AnalysisMtx);
  if (!AnalysisRes.count(F)) {
    return;
  }

  auto *Smry = AnalysisRes.at(F).get();
  assert(Smry->getFunc() == F);

  if (Smry->isSolverShared()) {
    // Shared summary solvers outlive individual summaries. Lock the solver
    // before erasing SMT-owned state so another thread cannot read half-torn
    // expressions while initializing a dependent summary.
    auto *Solver = Smry->getSmrySolver();
    // Erasing summary will destruct exprs constructed by the shared smry Solver
    // Another thread may be trying to init summary using this solver.
    std::lock_guard<std::mutex> Lock2(Solver->getSolverLock());
    AnalysisRes.erase(F);
  } else {
    // release solver after releasing smt exprs
    AnalysisRes.erase(F);
    SummarySolverManager::get().releaseFuncSolver(F);
  }
}

void AnalysisDriver::runOnFunction(GuardedValueFlowGraph *Graph) {
  Function *F = Graph->getBaseFunction();

  if (AllocatorAPI::get(F)) {
    return;
  }

  std::vector<BasicBlock *> topBBs;
  {
    std::lock_guard<std::mutex> Lock(AnalysisMtx);
    topsortCFG(topBBs, F);
  }

  // Each function gets a fresh AnalysisState, but the driver supplies the
  // module-wide summary and trace plumbing around it.
  AnalysisState State(static_cast<AnalysisState::SymexBugType>(BugTy), Graph,
                      F);
  for (auto *BB : topBBs) {
    for (Instruction &I : *BB) {
      State.transfer(&I, *this);
    }
  }

  State.finalizeSummary();

  const auto &BugReports = State.getBugReports();
  if (!BugReports.empty()) {
    for (const auto &TraceP : BugReports) {
      const auto &BugTy = std::get<0>(TraceP);
      const auto &TaintSteps = std::get<1>(TraceP);
      const auto &QuerySteps = std::get<2>(TraceP);

      std::tuple<AnalysisState::SymexBugType, std::vector<TaintStep>,
                 std::vector<TraceStep>>
          CurTrace = std::make_tuple(BugTy, TaintSteps, QuerySteps);

      {
        std::lock_guard<std::mutex> Lock(AnalysisMtx);
        auto *SinkPos = QuerySteps.front().Inst;
        // Multiple paths can collapse onto the same sink instruction once the
        // state has been summarized. Keep the first trace per sink so the final
        // wrapper emits one stable report instead of a burst of
        // near-duplicates.
        if (!SinkInsts.count(SinkPos)) {
          SinkInsts.insert(SinkPos);
          Traces.emplace_back(CurTrace);
        }
      }
    }
  }

  addSummary(F, AnalysisSummary(std::move(State)));
}
