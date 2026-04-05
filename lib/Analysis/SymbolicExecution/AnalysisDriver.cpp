

#include "Analysis/SymbolicExecution/AnalysisDriver.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"

#include "Analysis/SymbolicExecution/MemoryAPI.h"
#include "Analysis/SymbolicExecution/SegUtility.h"

#include <algorithm>
#include <functional>
#include <numeric>
#include <sstream>
#include <string>

using namespace SymbolicExecution;

namespace {

static void topsortCFG(std::vector<BasicBlock *> &sorted, Function *F) {
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

// External reference to enable-symex-bof flag from cb-check.cpp
extern cl::opt<bool> enable_symex_bof_engine;

static cl::opt<bool> SymexEnableCheckBof(
    "symex-bof",
    cl::desc("Check buffer overflow under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckDbz(
    "symex-dbz",
    cl::desc("Check divide-by-zero under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckIntOverflow(
    "symex-int-overflow",
    cl::desc("Check integer overflow under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckIntUnderflow(
    "symex-int-underflow",
    cl::desc("Check integer underflow under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckNullDeref(
    "symex-null-deref",
    cl::desc(
        "Check null pointer dereference under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckSignedIntOverflow(
    "symex-signed-int-overflow",
    cl::desc(
        "Check signed integer overflow under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckSignedIntUnderflow(
    "symex-signed-int-underflow",
    cl::desc(
        "Check signed integer underflow under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckShiftOverflow(
    "symex-shift-overflow",
    cl::desc(
        "Check shift overflow/underflow under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckArrayIndexOOB(
    "symex-array-index-oob",
    cl::desc(
        "Check array index out of bounds under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckUninitRead(
    "symex-uninit-read",
    cl::desc(
        "Check uninitialized memory read under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckUaf(
    "symex-uaf",
    cl::desc("Check use-after-free under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckDoubleFree(
    "symex-double-free",
    cl::desc("Check double-free under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckNegativeArrayIndex(
    "symex-negative-array-index",
    cl::desc(
        "Check negative array indexing under the symbolic execution engine"),
    cl::init(false));

static cl::opt<bool> SymexEnableCheckIntTruncation(
    "symex-int-truncation",
    cl::desc("Check integer truncation/conversion issues under the symbolic "
             "execution engine"),
    cl::init(false));

static cl::opt<std::string> SymexCheckers(
    "symex-checkers",
    cl::desc("Comma-separated list of checkers to enable. Available checkers: "
             "bof, dbz, int-overflow, int-underflow, null-deref, "
             "signed-int-overflow, signed-int-underflow, shift-overflow, "
             "array-index-oob, uninit-read, uaf, double-free, "
             "negative-array-index, int-truncation"),
    cl::init(""));

AnalysisDriver::AnalysisDriver() { initBugType(); }

void AnalysisDriver::initBugType() {
  // Parse comma-separated list of checkers if provided
  if (!SymexCheckers.empty()) {
    std::string checkers = SymexCheckers;
    // Remove whitespace
    checkers.erase(std::remove_if(checkers.begin(), checkers.end(), ::isspace),
                   checkers.end());

    std::stringstream ss(checkers);
    std::string checker;
    while (std::getline(ss, checker, ',')) {
      if (checker == "bof") {
        BugTy |= AnalysisState::BUG_TY_BOF;
      } else if (checker == "dbz") {
        BugTy |= AnalysisState::BUG_TY_DBZ;
      } else if (checker == "int-overflow") {
        BugTy |= AnalysisState::BUG_TY_INT_OVERFLOW;
      } else if (checker == "int-underflow") {
        BugTy |= AnalysisState::BUG_TY_INT_UNDERFLOW;
      } else if (checker == "null-deref") {
        BugTy |= AnalysisState::BUG_TY_NULL_DEREF;
      } else if (checker == "signed-int-overflow") {
        BugTy |= AnalysisState::BUG_TY_SIGNED_INT_OVERFLOW;
      } else if (checker == "signed-int-underflow") {
        BugTy |= AnalysisState::BUG_TY_SIGNED_INT_UNDERFLOW;
      } else if (checker == "shift-overflow") {
        BugTy |= AnalysisState::BUG_TY_SHIFT_OVERFLOW;
      } else if (checker == "array-index-oob") {
        BugTy |= AnalysisState::BUG_TY_ARRAY_INDEX_OOB;
      } else if (checker == "uninit-read") {
        BugTy |= AnalysisState::BUG_TY_UNINIT_READ;
      } else if (checker == "uaf") {
        BugTy |= AnalysisState::BUG_TY_UAF;
      } else if (checker == "double-free") {
        BugTy |= AnalysisState::BUG_TY_DOUBLE_FREE;
      } else if (checker == "negative-array-index") {
        BugTy |= AnalysisState::BUG_TY_NEGATIVE_ARRAY_INDEX;
      } else if (checker == "int-truncation") {
        BugTy |= AnalysisState::BUG_TY_INT_TRUNCATION;
      } else {
        llvm::errs() << "Warning: Unknown checker '" << checker
                     << "' in --symex-checkers. Ignoring.\n";
      }
    }
  }

  // Also check individual flags (these can be used together with
  // --symex-checkers)
  if (SymexEnableCheckBof) {
    BugTy |= AnalysisState::BUG_TY_BOF;
  }

  if (SymexEnableCheckDbz) {
    BugTy |= AnalysisState::BUG_TY_DBZ;
  }

  if (SymexEnableCheckIntOverflow) {
    BugTy |= AnalysisState::BUG_TY_INT_OVERFLOW;
  }

  if (SymexEnableCheckIntUnderflow) {
    BugTy |= AnalysisState::BUG_TY_INT_UNDERFLOW;
  }

  if (SymexEnableCheckNullDeref) {
    BugTy |= AnalysisState::BUG_TY_NULL_DEREF;
  }

  if (SymexEnableCheckSignedIntOverflow) {
    BugTy |= AnalysisState::BUG_TY_SIGNED_INT_OVERFLOW;
  }

  if (SymexEnableCheckSignedIntUnderflow) {
    BugTy |= AnalysisState::BUG_TY_SIGNED_INT_UNDERFLOW;
  }

  if (SymexEnableCheckShiftOverflow) {
    BugTy |= AnalysisState::BUG_TY_SHIFT_OVERFLOW;
  }

  if (SymexEnableCheckArrayIndexOOB) {
    BugTy |= AnalysisState::BUG_TY_ARRAY_INDEX_OOB;
  }

  if (SymexEnableCheckUninitRead) {
    BugTy |= AnalysisState::BUG_TY_UNINIT_READ;
  }

  if (SymexEnableCheckUaf) {
    BugTy |= AnalysisState::BUG_TY_UAF;
  }

  if (SymexEnableCheckDoubleFree) {
    BugTy |= AnalysisState::BUG_TY_DOUBLE_FREE;
  }

  if (SymexEnableCheckNegativeArrayIndex) {
    BugTy |= AnalysisState::BUG_TY_NEGATIVE_ARRAY_INDEX;
  }

  if (SymexEnableCheckIntTruncation) {
    BugTy |= AnalysisState::BUG_TY_INT_TRUNCATION;
  }

  // If enable-symex-bof is used and no checkers are specified, default to BOF
  // This provides backward compatibility
  if (BugTy == AnalysisState::BUG_TY_UNDEF) {
    if (enable_symex_bof_engine.getValue()) {
      BugTy = AnalysisState::BUG_TY_BOF;
    } else {
      // If enable-symex-bof is not used, still default to BOF for backward
      // compatibility
      BugTy = AnalysisState::BUG_TY_BOF;
    }
  }
}

void AnalysisDriver::runOnModuleParallel(Module *M) {
  // Lotus does not carry the old Clearblue thread-pool dependency used here.
  // Keep a correct migration by reusing the sequential execution path until a
  // native parallel scheduler is reintroduced for symex.
  runOnModule(M);
}

void AnalysisDriver::runOnModule(Module *M) {
  AnalysisState::NON_PTR_TY = Type::getInt64Ty(M->getContext());
  AnalysisState::INT8_TY = Type::getInt8Ty(M->getContext());
  SummarySolverManager::get().init();

  seg_utility::getTopoOrder(*M);
  const auto &FuncSeq = seg_utility::getFuncSeq();
  assert(!FuncSeq.empty());

  // The heartbleed bug: "ssl3_read_bytes"
  // Function *TargerFunc = M->getFunction("ssl3_read_bytes");
  // assert(TargerFunc);
  // auto TargetSlice = seg_utility::getCallSlice(TargerFunc);

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
    auto *Graph = seg_utility::getGraph(CurFunc);

    if (!Graph) {
      continue;
    }

    unsigned FunDepth = seg_utility::getFunctionDepth(CurFunc);
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
        if (!SinkInsts.count(SinkPos)) {
          SinkInsts.insert(SinkPos);
          Traces.emplace_back(CurTrace);
        }
      }
    }
  }

  addSummary(F, AnalysisSummary(std::move(State)));
}
