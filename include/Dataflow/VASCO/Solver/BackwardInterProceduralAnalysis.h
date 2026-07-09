#pragma once

#include "Dataflow/VASCO/Core/InterProceduralAnalysis.h"
#include "Utils/Parallel/ThreadPool.h"

#include <algorithm>
#include <optional>
#include <set>
#include <thread>

namespace vasco {

template <typename M, typename N, typename A>
class BackwardInterProceduralAnalysis
    : public InterProceduralAnalysis<M, N, A> {
public:
  using Base = InterProceduralAnalysis<M, N, A>;
  using typename Base::CallSiteType;
  using typename Base::ContextPtr;

  BackwardInterProceduralAnalysis() : Base(true) {}

  void doAnalysis() override {
    for (const auto &Method : this->programRepresentation().getEntryPoints()) {
      initContext(Method, this->boundaryValue(Method));
    }

    if (this->SchedulingOptions.EnableParallelContextScheduling &&
        !this->FreeResultsOnTheFly && ThreadPool::get()->hasWorkers()) {
      runParallelAnalysis();
    } else {
      runSequentialAnalysis();
    }

    this->sanityCheckAnalysedContexts();
  }

protected:
  void runSequentialAnalysis() {
    while (this->hasPendingContexts()) {
      auto CurrentContext = this->newestContext();
      processContextStep(CurrentContext);
    }
  }

  void runParallelAnalysis() {
    ThreadPool *Pool = ThreadPool::get();
    ThreadPool::TaskGroup Group = Pool->makeTaskGroup();
    std::set<ContextPtr, ContextPtrComparator<M, N, A>> RunningContexts;
    const unsigned Workers = std::max<unsigned>(1, Pool->workerCount());

    for (unsigned I = 0; I < Workers; ++I) {
      Group.async([this, &RunningContexts] {
        this->recordParallelWorkerTask();
        while (true) {
          auto CurrentContext = this->takeRunnableContext(RunningContexts);
          if (!CurrentContext) {
            if (this->parallelWorkComplete(RunningContexts)) {
              break;
            }
            std::this_thread::yield();
            continue;
          }

          bool HasMoreLocalWork = false;
          {
            std::lock_guard<std::recursive_mutex> Lock(CurrentContext->mutex());
            HasMoreLocalWork = processContextBatch(CurrentContext);
          }
          this->finishRunnableContext(CurrentContext, HasMoreLocalWork,
                                      RunningContexts);
        }
      });
    }

    Group.wait();
  }

  bool processContextBatch(ContextPtr CurrentContext) {
    const std::size_t Budget = this->SchedulingOptions.ContextStepBudget == 0
                                   ? 1
                                   : this->SchedulingOptions.ContextStepBudget;
    std::size_t Steps = 0;
    do {
      processContextStep(CurrentContext);
      ++Steps;
    } while (Steps < Budget && !CurrentContext->getWorkList().empty());
    this->recordContextBatch();
    return !CurrentContext->getWorkList().empty();
  }

  void processContextStep(ContextPtr CurrentContext) {
    if (CurrentContext->getWorkList().empty()) {
      this->removeContextFromWorklist(CurrentContext);
      return;
    }

    auto ItemIt = CurrentContext->getWorkList().begin();
    const auto Item = *ItemIt;
    CurrentContext->getWorkList().erase(ItemIt);
    this->recordContextStep();

    if (Item.has_value()) {
      const auto &Node = *Item;
      const auto Graph = CurrentContext->getControlFlowGraph();

      const auto Successors = Graph->succsOf(Node);
      if (!Successors.empty()) {
        A Out = this->topValue();
        for (const auto &Successor : Successors) {
          Out = this->meet(Out, CurrentContext->getValueBefore(Successor));
        }
        CurrentContext->setValueAfter(Node, Out);
      }

      const A PrevIn = CurrentContext->getValueBefore(Node);
      const A Out = CurrentContext->getValueAfter(Node);

      A In = this->topValue();
      if (this->programRepresentation().isCall(Node)) {
        bool Hit = false;
        const auto MaybeTargets = this->programRepresentation().resolveTargets(
            CurrentContext->getMethod(), Node);
        if (!MaybeTargets.has_value()) {
          this->addTransition(CallSiteType(CurrentContext, Node), nullptr);
          In = unknownCallFlowFunction(CurrentContext, Node, Out);
        } else if (MaybeTargets->empty()) {
          In = callLocalFlowFunction(CurrentContext, Node, Out);
        } else {
          for (const auto &TargetMethod : *MaybeTargets) {
            const A ExitValue =
                callExitFlowFunction(CurrentContext, TargetMethod, Node, Out);
            CallSiteType CallSite(CurrentContext, Node);

            auto TargetContext =
                getOrInitTargetContext(TargetMethod, ExitValue);

            this->addTransition(CallSite, TargetContext);

            if (TargetContext->isAnalysed()) {
              Hit = true;
              this->observeSummaryVersion(CallSite, TargetContext);
              const A EntryValue = TargetContext->getEntryValue();
              const A CallValue = callEntryFlowFunction(
                  CurrentContext, TargetMethod, Node, EntryValue);
              In = this->meet(In, CallValue);
            }
          }
        }

        if (Hit) {
          In = this->meet(In, callLocalFlowFunction(CurrentContext, Node, Out));
        }
      } else {
        In = normalFlowFunction(CurrentContext, Node, Out);
      }

      In = this->meet(In, PrevIn);
      CurrentContext->setValueBefore(Node, In);

      if (!(In == PrevIn)) {
        for (const auto &Predecessor : Graph->predsOf(Node)) {
          CurrentContext->getWorkList().insert(Predecessor);
        }
      }

      for (const auto &Head : Graph->heads()) {
        if (Head == Node) {
          CurrentContext->getWorkList().insert(std::nullopt);
          break;
        }
      }
    } else {
      const auto Graph = CurrentContext->getControlFlowGraph();
      A EntryValue = this->topValue();
      for (const auto &HeadNode : Graph->heads()) {
        EntryValue =
            this->meet(EntryValue, CurrentContext->getValueBefore(HeadNode));
      }

      const bool WasAnalysed = CurrentContext->isAnalysed();
      const A PreviousEntryValue = CurrentContext->getEntryValue();
      const bool SummaryChanged = !(EntryValue == PreviousEntryValue);
      CurrentContext->setEntryValue(EntryValue);
      CurrentContext->markAnalysed();
      if (!WasAnalysed || SummaryChanged) {
        CurrentContext->publishSummaryVersion();
        this->recordSummaryPublication();
        this->wakeStaleCallers(CurrentContext);
      } else {
        this->recordSuppressedUnchangedWakeup();
      }
      this->freeReachableContextsIfDead(CurrentContext);
    }
  }

  ContextPtr getOrInitTargetContext(const M &Method, const A &ExitValue) {
    std::lock_guard<std::recursive_mutex> Lock(this->StateMutex);
    auto TargetContext = this->getContextUnlocked(Method, ExitValue);
    if (TargetContext) {
      this->recordContextReuse();
      return TargetContext;
    }
    return initContext(Method, ExitValue);
  }

  ContextPtr initContext(const M &Method, const A &ExitValue) {
    auto NewContext = std::make_shared<vasco::Context<M, N, A>>(
        Method, this->programRepresentation().getControlFlowGraph(Method),
        true);

    const auto Graph = NewContext->getControlFlowGraph();
    for (const auto &Node : Graph->nodes()) {
      NewContext->setValueBefore(Node, this->topValue());
      NewContext->setValueAfter(Node, this->topValue());
      NewContext->getWorkList().insert(Node);
    }

    NewContext->setExitValue(this->copy(ExitValue));
    for (const auto &Tail : Graph->tails()) {
      NewContext->setValueAfter(Tail, this->copy(ExitValue));
    }
    NewContext->setEntryValue(this->topValue());

    this->registerContext(NewContext);
    this->enqueueContext(NewContext);
    return NewContext;
  }

  virtual A normalFlowFunction(ContextPtr Context, const N &Node,
                               const A &OutValue) = 0;
  virtual A callEntryFlowFunction(ContextPtr Context, const M &TargetMethod,
                                  const N &Node, const A &EntryValue) = 0;
  virtual A callExitFlowFunction(ContextPtr Context, const M &TargetMethod,
                                 const N &Node, const A &OutValue) = 0;
  virtual A callLocalFlowFunction(ContextPtr Context, const N &Node,
                                  const A &OutValue) = 0;
  virtual A unknownCallFlowFunction(ContextPtr Context, const N &Node,
                                    const A &OutValue) {
    return callLocalFlowFunction(Context, Node, OutValue);
  }
};

} // namespace vasco
