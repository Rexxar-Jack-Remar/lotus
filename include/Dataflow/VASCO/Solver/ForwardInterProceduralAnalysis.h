#pragma once

#include "Dataflow/VASCO/Core/InterProceduralAnalysis.h"
#include "Utils/Parallel/ThreadPool.h"

#include <algorithm>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace vasco {

template <typename M, typename N, typename A>
class ForwardInterProceduralAnalysis : public InterProceduralAnalysis<M, N, A> {
public:
  using Base = InterProceduralAnalysis<M, N, A>;
  using typename Base::CallSiteType;
  using typename Base::ContextPtr;

  ForwardInterProceduralAnalysis() : Base(false) {}

  void doAnalysis() override {
    for (const auto &Method : this->programRepresentation().getEntryPoints()) {
      if (this->programRepresentation().isPhantomMethod(Method)) {
        initContextForPhantomMethod(Method, this->boundaryValue(Method));
      } else {
        initContext(Method, this->boundaryValue(Method));
      }
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
      const auto PublishedVersion = processContextStep(CurrentContext);
      if (PublishedVersion.has_value()) {
        this->wakeStaleCallers(CurrentContext, *PublishedVersion);
      }
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

          BatchResult Result;
          {
            std::lock_guard<std::recursive_mutex> Lock(CurrentContext->mutex());
            Result = processContextBatch(CurrentContext);
          }
          // Caller replay may lock other contexts, so it must happen after the
          // publishing context lock has been released.
          for (const std::size_t Version : Result.PublishedVersions) {
            this->wakeStaleCallers(CurrentContext, Version);
          }
          this->finishRunnableContext(CurrentContext, Result.HasMoreLocalWork,
                                      RunningContexts);
        }
      });
    }

    Group.wait();
  }

  struct BatchResult {
    bool HasMoreLocalWork = false;
    std::vector<std::size_t> PublishedVersions;
  };

  BatchResult processContextBatch(ContextPtr CurrentContext) {
    const std::size_t Budget = this->SchedulingOptions.ContextStepBudget == 0
                                   ? 1
                                   : this->SchedulingOptions.ContextStepBudget;
    std::size_t Steps = 0;
    BatchResult Result;
    do {
      const auto PublishedVersion = processContextStep(CurrentContext);
      if (PublishedVersion.has_value()) {
        Result.PublishedVersions.push_back(*PublishedVersion);
      }
      ++Steps;
    } while (Steps < Budget && !CurrentContext->getWorkList().empty());
    this->recordContextBatch();
    Result.HasMoreLocalWork = !CurrentContext->getWorkList().empty();
    return Result;
  }

  std::optional<std::size_t> processContextStep(ContextPtr CurrentContext) {
    if (CurrentContext->getWorkList().empty()) {
      this->removeContextFromWorklist(CurrentContext);
      if (CurrentContext->getSummarySnapshot()) {
        CurrentContext->markAnalysed();
        return std::nullopt;
      }
      const std::size_t PublishedVersion =
          CurrentContext->publishSummary(CurrentContext->getExitValue());
      this->recordSummaryPublication();
      return PublishedVersion;
    }

    auto ItemIt = CurrentContext->getWorkList().begin();
    const auto Item = *ItemIt;
    CurrentContext->getWorkList().erase(ItemIt);
    this->recordContextStep();

    if (Item.has_value()) {
      const auto &Node = *Item;
      const auto Graph = CurrentContext->getControlFlowGraph();

      const auto Predecessors = Graph->predsOf(Node);
      if (!Predecessors.empty()) {
        A In = this->topValue();
        for (const auto &Pred : Predecessors) {
          In = this->meet(In, CurrentContext->getValueAfter(Pred));
        }
        CurrentContext->setValueBefore(Node, In);
      }

      const A PrevOut = CurrentContext->getValueAfter(Node);
      const A In = CurrentContext->getValueBefore(Node);

      A Out = this->topValue();
      if (this->programRepresentation().isCall(Node)) {
        bool Hit = false;
        const auto MaybeTargets = this->programRepresentation().resolveTargets(
            CurrentContext->getMethod(), Node);
        if (!MaybeTargets.has_value()) {
          this->addTransition(CallSiteType(CurrentContext, Node), nullptr);
          Out = unknownCallFlowFunction(CurrentContext, Node, In);
        } else if (!MaybeTargets->empty()) {
          for (const auto &TargetMethod : *MaybeTargets) {
            const A EntryValue =
                callEntryFlowFunction(CurrentContext, TargetMethod, Node, In);
            CallSiteType CallSite(CurrentContext, Node);

            auto TargetContext =
                getOrInitTargetContext(TargetMethod, EntryValue);

            this->addTransition(CallSite, TargetContext);

            const auto Summary = TargetContext->getSummarySnapshot();
            if (Summary) {
              Hit = true;
              this->observeSummaryVersion(CallSite, TargetContext,
                                          Summary->Version);
              const A ReturnedValue = callExitFlowFunction(
                  CurrentContext, TargetMethod, Node, Summary->Value);
              Out = this->meet(Out, ReturnedValue);
            }
          }

          if (Hit) {
            Out = this->meet(Out,
                             callLocalFlowFunction(CurrentContext, Node, In));
          } else {
            Out = callLocalFlowFunction(CurrentContext, Node, In);
          }
        } else {
          Out = callLocalFlowFunction(CurrentContext, Node, In);
        }
      } else {
        Out = normalFlowFunction(CurrentContext, Node, In);
      }

      Out = this->meet(Out, PrevOut);
      CurrentContext->setValueAfter(Node, Out);

      if (!(Out == PrevOut)) {
        for (const auto &Successor : Graph->succsOf(Node)) {
          CurrentContext->getWorkList().insert(Successor);
        }
      }

      for (const auto &Tail : Graph->tails()) {
        if (Tail == Node) {
          CurrentContext->getWorkList().insert(std::nullopt);
          break;
        }
      }
    } else {
      const auto Graph = CurrentContext->getControlFlowGraph();
      A ExitValue = this->topValue();
      for (const auto &TailNode : Graph->tails()) {
        ExitValue =
            this->meet(ExitValue, CurrentContext->getValueAfter(TailNode));
      }

      const auto PreviousSummary = CurrentContext->getSummarySnapshot();
      const A PreviousExitValue = CurrentContext->getExitValue();
      const bool SummaryChanged = !(ExitValue == PreviousExitValue);
      CurrentContext->setExitValue(ExitValue);
      if (!PreviousSummary || SummaryChanged) {
        const std::size_t PublishedVersion =
            CurrentContext->publishSummary(ExitValue);
        this->recordSummaryPublication();
        this->freeReachableContextsIfDead(CurrentContext);
        return PublishedVersion;
      } else {
        CurrentContext->markAnalysed();
        this->recordSuppressedUnchangedWakeup();
      }
      this->freeReachableContextsIfDead(CurrentContext);
    }
    return std::nullopt;
  }

  ContextPtr getOrInitTargetContext(const M &Method, const A &EntryValue) {
    std::lock_guard<std::recursive_mutex> Lock(this->StateMutex);
    auto TargetContext = this->getContextUnlocked(Method, EntryValue);
    if (TargetContext) {
      this->recordContextReuse();
      return TargetContext;
    }
    if (this->programRepresentation().isPhantomMethod(Method)) {
      return initContextForPhantomMethod(Method, EntryValue);
    }
    return initContext(Method, EntryValue);
  }

  ContextPtr initContextForPhantomMethod(const M &Method, const A &EntryValue) {
    auto NewContext = std::make_shared<vasco::Context<M, N, A>>(Method);
    NewContext->setEntryValue(this->copy(EntryValue));
    NewContext->setExitValue(this->copy(EntryValue));
    NewContext->publishSummary(NewContext->getExitValue());
    this->registerContext(NewContext);
    return NewContext;
  }

  ContextPtr initContext(const M &Method, const A &EntryValue) {
    auto NewContext = std::make_shared<vasco::Context<M, N, A>>(
        Method, this->programRepresentation().getControlFlowGraph(Method),
        false);

    const auto Graph = NewContext->getControlFlowGraph();
    for (const auto &Node : Graph->nodes()) {
      NewContext->setValueBefore(Node, this->topValue());
      NewContext->setValueAfter(Node, this->topValue());
      NewContext->getWorkList().insert(Node);
    }

    NewContext->setEntryValue(this->copy(EntryValue));
    for (const auto &Head : Graph->heads()) {
      NewContext->setValueBefore(Head, this->copy(EntryValue));
    }
    NewContext->setExitValue(this->topValue());

    this->registerContext(NewContext);
    this->enqueueContext(NewContext);
    return NewContext;
  }

  virtual A normalFlowFunction(ContextPtr Context, const N &Node,
                               const A &InValue) = 0;
  virtual A callEntryFlowFunction(ContextPtr Context, const M &TargetMethod,
                                  const N &Node, const A &InValue) = 0;
  virtual A callExitFlowFunction(ContextPtr Context, const M &TargetMethod,
                                 const N &Node, const A &ExitValue) = 0;
  virtual A callLocalFlowFunction(ContextPtr Context, const N &Node,
                                  const A &InValue) = 0;
  virtual A unknownCallFlowFunction(ContextPtr Context, const N &Node,
                                    const A &InValue) {
    return callLocalFlowFunction(Context, Node, InValue);
  }
};

} // namespace vasco
