#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <stdexcept>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "PipelineScheduler"

using namespace llvm;

static cl::opt<int>
    TaskTimeout("scheduler-task-timeout",
                cl::desc("Timeout for avoiding deadlock (in seconds)"),
                cl::ValueOptional, cl::init(60), cl::ReallyHidden);

namespace {

class SchedulerTimeoutError : public std::runtime_error {
public:
  explicit SchedulerTimeoutError(const std::string &Message)
      : std::runtime_error(Message) {}
};

} // namespace

static inline bool shouldAnalyzeFunction(const Function *Func) {
  return Func && !Func->isIntrinsic() && !Func->isDeclaration();
}

PipelineScheduler::PipelineScheduler(Module &M, CallGraph &CG, AnalysisType AT)
    : M(M), CG(CG), AType(AT),
      Prog("[Pipeline Scheduler]", ProgressBar::PBS_CharacterStyle),
      ClientContext(nullptr), TaskTimeout(::TaskTimeout.getValue()),
      EnableGC(true), GCBatchSize(100) {
  int FuncIndex = 0;
  for (auto &F : M) {
    if (!shouldAnalyzeFunction(&F))
      continue;
    Functions.push_back(&F);
    FunctionIndexMap[&F] = FuncIndex++;
  }

  FunctionCalleeIndexVec.resize(Functions.size());

  LLVM_DEBUG(dbgs() << "[PipelineScheduler] Total functions: "
                    << Functions.size() << "\n");

  if (AType != AT_Local) {
    buildFunctionGraph();
    computeSCCs();
    buildSCCDAG();
  }
}

PipelineScheduler::~PipelineScheduler() = default;

void PipelineScheduler::finishTask(std::shared_ptr<Task> T) {
  LLVM_DEBUG(dbgs() << "[PipelineScheduler] Task " << T->toString()
                    << " finished\n");
  {
    std::unique_lock<std::mutex> Lock(FTVecMutex);
    FinishedTaskVec.push_back(std::move(T));
  }
  FTVecCond.notify_one();
}

void PipelineScheduler::recordTaskFailure(std::exception_ptr Failure) {
  if (!Failure)
    return;

  std::lock_guard<std::mutex> Lock(FailureMutex);
  if (!TaskFailure)
    TaskFailure = Failure;
}

std::exception_ptr PipelineScheduler::getTaskFailure() {
  std::lock_guard<std::mutex> Lock(FailureMutex);
  return TaskFailure;
}

void PipelineScheduler::run() {
  if (!TaskCallback) {
    errs() << "Error: TaskCallback not set! Call setTaskCallback() before "
              "run().\n";
    return;
  }

  llvm::outs() << "Starting pipeline scheduler...\n";
  ExecutionCancellation = lotus::CancellationSource();
  ExecutionGroup =
      std::make_unique<ThreadPool::TaskGroup>(ThreadPool::get()->makeTaskGroup());

  if (AType == AT_Local) {
    for (const auto *F : Functions) {
      auto FTask =
          std::make_shared<FunctionTask>(F, TaskCallback, ClientContext);
      executeTask(FTask);
    }
  } else {
    for (std::size_t SCCIndex = 0; SCCIndex < SCCs.size(); ++SCCIndex) {
      if (SCCs[SCCIndex].RemainingScheduleDeps != 0)
        continue;
      auto SCCTask = std::make_shared<SCCFunctionTask>(
          static_cast<int>(SCCIndex), getOrderedSCCFunctions(SCCIndex),
          TaskCallback, ClientContext);
      executeTask(SCCTask);
    }
  }

  waitTask();

  if (ExecutionGroup) {
    try {
      ExecutionGroup->wait();
    } catch (...) {
      recordTaskFailure(std::current_exception());
    }
    ExecutionGroup.reset();
  }

  if (std::exception_ptr Failure = getTaskFailure())
    std::rethrow_exception(Failure);

  llvm::outs() << "\nPipeline scheduler completed!\n";
}

void PipelineScheduler::executeTask(std::shared_ptr<Task> T) {
  assert(ExecutionGroup && "scheduler execution group must be initialized");
  const lotus::CancellationToken Token = ExecutionCancellation.token();
  ExecutionGroup->async(Token, [T, this, Token]() {
    if (Token.isCancelled()) {
      finishTask(T);
      return;
    }

    try {
      T->run();
    } catch (...) {
      recordTaskFailure(std::current_exception());
      ExecutionCancellation.cancel();
      finishTask(T);
      throw;
    }
    finishTask(T);
  });
}

void PipelineScheduler::waitTask() {
  const std::size_t NumAllTasks =
      (AType == AT_Local) ? Functions.size() : SCCs.size();
  std::size_t NumUnfinishedTasks = NumAllTasks;
  std::size_t NumGCTasks = 0;

  while (NumUnfinishedTasks || NumGCTasks) {
    LLVM_DEBUG(dbgs() << "[PipelineScheduler] Unfinished tasks: "
                      << NumUnfinishedTasks << "\n");

    std::shared_ptr<Task> T;
    {
      std::unique_lock<std::mutex> Lock(FTVecMutex);
      FTVecCond.wait_for(Lock, std::chrono::seconds(TaskTimeout * 2),
                         [this] { return !FinishedTaskVec.empty(); });

      if (FinishedTaskVec.empty()) {
        Prog.showProgress(1);
        ExecutionCancellation.cancel();
        recordTaskFailure(std::make_exception_ptr(SchedulerTimeoutError(
            "PipelineScheduler timed out waiting for tasks")));
        errs() << "\nError: Timeout waiting for tasks; cancelling "
                  "outstanding work.\n";
        break;
      }

      T = FinishedTaskVec.back();
      FinishedTaskVec.pop_back();
    }

    if (isa<GCTask>(T.get())) {
      assert(NumGCTasks != 0 && "GC task accounting underflow");
      --NumGCTasks;
      continue;
    }

    assert(NumUnfinishedTasks != 0 && "task accounting underflow");
    --NumUnfinishedTasks;

    if (!getTaskFailure()) {
      if (auto *SCCTask = dyn_cast<SCCFunctionTask>(T.get()))
        NumGCTasks += postProcessSCCFunctionTask(
            std::static_pointer_cast<SCCFunctionTask>(T));
    }

    if (NumAllTasks != 0)
      Prog.showProgress(static_cast<float>(NumAllTasks - NumUnfinishedTasks) /
                        static_cast<float>(NumAllTasks));
    else
      Prog.showProgress(1);
  }

  if (getTaskFailure())
    ExecutionCancellation.cancel();

  if (!getTaskFailure() && EnableGC && GCCallback && !FunctionToRelease.empty()) {
    GCTask TrailingGC(FunctionToRelease, GCCallback, ClientContext);
    TrailingGC.run();
    FunctionToRelease.clear();
  }

  llvm::outs() << "\n";
}

int PipelineScheduler::postProcessSCCFunctionTask(
    std::shared_ptr<SCCFunctionTask> T) {
  const int SCCIndex = T->getSCCIndex();
  int NumGCTasksAdded = 0;

  if (AType == AT_BottomUp) {
    for (int CallerSCC : SCCs[SCCIndex].Callers) {
      auto &Caller = SCCs[CallerSCC];
      assert(Caller.RemainingScheduleDeps != 0 &&
             "bottom-up dependency underflow");
      --Caller.RemainingScheduleDeps;
      if (Caller.RemainingScheduleDeps == 0) {
        auto ReadyTask = std::make_shared<SCCFunctionTask>(
            CallerSCC, getOrderedSCCFunctions(CallerSCC), TaskCallback,
            ClientContext);
        executeTask(ReadyTask);
      }
    }
  } else if (AType == AT_TopDown) {
    for (int CalleeSCC : SCCs[SCCIndex].Callees) {
      auto &Callee = SCCs[CalleeSCC];
      assert(Callee.RemainingScheduleDeps != 0 &&
             "top-down dependency underflow");
      --Callee.RemainingScheduleDeps;
      if (Callee.RemainingScheduleDeps == 0) {
        auto ReadyTask = std::make_shared<SCCFunctionTask>(
            CalleeSCC, getOrderedSCCFunctions(CalleeSCC), TaskCallback,
            ClientContext);
        executeTask(ReadyTask);
      }
    }
  }

  if (EnableGC && GCCallback) {
    if (SCCs[SCCIndex].RemainingCallersForGC == 0)
      maybeReleaseSCC(SCCIndex, NumGCTasksAdded);

    for (int CalleeSCC : SCCs[SCCIndex].Callees) {
      auto &Callee = SCCs[CalleeSCC];
      assert(Callee.RemainingCallersForGC != 0 && "GC dependency underflow");
      --Callee.RemainingCallersForGC;
      if (Callee.RemainingCallersForGC == 0)
        maybeReleaseSCC(CalleeSCC, NumGCTasksAdded);
    }
  }

  return NumGCTasksAdded;
}

void PipelineScheduler::buildFunctionGraph() {
  for (std::size_t CallerIndex = 0; CallerIndex < Functions.size();
       ++CallerIndex) {
    const Function *Caller = Functions[CallerIndex];
    CallGraphNode *CallerNode = CG[const_cast<Function *>(Caller)];
    if (!CallerNode)
      continue;

    auto &Callees = FunctionCalleeIndexVec[CallerIndex];
    for (auto &CallRecord : *CallerNode) {
      Function *Callee = CallRecord.second->getFunction();
      if (!shouldAnalyzeFunction(Callee))
        continue;

      auto CalleeIt = FunctionIndexMap.find(Callee);
      if (CalleeIt == FunctionIndexMap.end())
        continue;

      Callees.insert(CalleeIt->second);
    }
  }
}

void PipelineScheduler::computeSCCs() {
  SCCs.clear();
  FunctionToSCC.assign(Functions.size(), -1);

  std::vector<int> Indices(Functions.size(), -1);
  std::vector<int> LowLinks(Functions.size(), -1);
  std::vector<int> Stack;
  std::vector<bool> OnStack(Functions.size(), false);
  int NextIndex = 0;

  auto FunctionOrder = [this](int LHS, int RHS) {
    const Function *LF = Functions[static_cast<std::size_t>(LHS)];
    const Function *RF = Functions[static_cast<std::size_t>(RHS)];
    if (LF->hasName() != RF->hasName())
      return LF->hasName() && !RF->hasName();
    if (LF->hasName() && RF->hasName() &&
        LF->getName() != RF->getName()) {
      return LF->getName() < RF->getName();
    }
    return FunctionIndexMap.at(LF) < FunctionIndexMap.at(RF);
  };

  std::function<void(int)> StrongConnect = [&](int V) {
    Indices[V] = NextIndex;
    LowLinks[V] = NextIndex;
    ++NextIndex;
    Stack.push_back(V);
    OnStack[V] = true;

    for (int W : FunctionCalleeIndexVec[static_cast<std::size_t>(V)]) {
      if (Indices[W] == -1) {
        StrongConnect(W);
        LowLinks[V] = std::min(LowLinks[V], LowLinks[W]);
      } else if (OnStack[W]) {
        LowLinks[V] = std::min(LowLinks[V], Indices[W]);
      }
    }

    if (LowLinks[V] != Indices[V])
      return;

    SCCNode Node;
    const int SCCIndex = static_cast<int>(SCCs.size());
    while (true) {
      int W = Stack.back();
      Stack.pop_back();
      OnStack[W] = false;
      Node.Members.push_back(W);
      FunctionToSCC[static_cast<std::size_t>(W)] = SCCIndex;
      if (W == V)
        break;
    }

    std::sort(Node.Members.begin(), Node.Members.end(), FunctionOrder);
    SCCs.push_back(std::move(Node));
  };

  for (std::size_t FunctionIndex = 0; FunctionIndex < Functions.size();
       ++FunctionIndex) {
    if (Indices[FunctionIndex] == -1)
      StrongConnect(static_cast<int>(FunctionIndex));
  }
}

void PipelineScheduler::buildSCCDAG() {
  std::vector<std::set<int>> SCCCallers(SCCs.size());
  std::vector<std::set<int>> SCCCallees(SCCs.size());

  for (std::size_t CallerIndex = 0; CallerIndex < Functions.size();
       ++CallerIndex) {
    const int CallerSCC = FunctionToSCC[CallerIndex];
    for (int CalleeIndex : FunctionCalleeIndexVec[CallerIndex]) {
      const int CalleeSCC = FunctionToSCC[static_cast<std::size_t>(CalleeIndex)];
      if (CallerSCC == CalleeSCC)
        continue;
      SCCCallees[CallerSCC].insert(CalleeSCC);
      SCCCallers[CalleeSCC].insert(CallerSCC);
    }
  }

  for (std::size_t SCCIndex = 0; SCCIndex < SCCs.size(); ++SCCIndex) {
    auto &Node = SCCs[SCCIndex];
    Node.Callers.assign(SCCCallers[SCCIndex].begin(), SCCCallers[SCCIndex].end());
    Node.Callees.assign(SCCCallees[SCCIndex].begin(), SCCCallees[SCCIndex].end());
    Node.RemainingCallersForGC = Node.Callers.size();
    Node.RemainingScheduleDeps =
        (AType == AT_BottomUp) ? Node.Callees.size() : Node.Callers.size();
  }
}

std::vector<const Function *>
PipelineScheduler::getOrderedSCCFunctions(int SCCIndex) const {
  std::vector<const Function *> OrderedFunctions;
  for (int FunctionIndex : SCCs[static_cast<std::size_t>(SCCIndex)].Members)
    OrderedFunctions.push_back(Functions[static_cast<std::size_t>(FunctionIndex)]);
  return OrderedFunctions;
}

void PipelineScheduler::maybeReleaseSCC(int SCCIndex, int &NumGCTasksAdded) {
  for (int FunctionIndex : SCCs[static_cast<std::size_t>(SCCIndex)].Members)
    FunctionToRelease.insert(Functions[static_cast<std::size_t>(FunctionIndex)]);

  if (FunctionToRelease.size() >= GCBatchSize)
    scheduleGCBatch(NumGCTasksAdded);
}

void PipelineScheduler::scheduleGCBatch(int &NumGCTasksAdded) {
  if (FunctionToRelease.empty())
    return;

  auto GTask =
      std::make_shared<GCTask>(FunctionToRelease, GCCallback, ClientContext);
  executeTask(GTask);
  FunctionToRelease.clear();
  ++NumGCTasksAdded;
}

void PipelineScheduler::dumpStatus() {
  std::unique_lock<std::mutex> Lock(FTVecMutex);
  llvm::outs() << "\n[PipelineScheduler Status]\n";
  llvm::outs() << "  Finished tasks in queue: " << FinishedTaskVec.size()
               << "\n";
  llvm::outs() << "  Functions to release: " << FunctionToRelease.size()
               << "\n";
  llvm::outs() << "  SCCs tracked: " << SCCs.size() << "\n";
}
