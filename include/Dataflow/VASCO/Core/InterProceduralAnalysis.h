#pragma once

#include "Dataflow/VASCO/Core/ContextTransitionTable.h"
#include "Dataflow/VASCO/Core/DataFlowSolution.h"
#include "Dataflow/VASCO/Core/ProgramRepresentation.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace vasco {

namespace detail {

template <typename T, typename = void>
struct IsOstreamWritable : std::false_type {};

template <typename T>
struct IsOstreamWritable<T, std::void_t<decltype(std::declval<std::ostream &>()
                                                 << std::declval<const T &>())>>
    : std::true_type {};

template <typename T>
void appendIfStreamable(std::ostream &OS, const T &Value) {
  if constexpr (IsOstreamWritable<T>::value) {
    OS << Value;
  }
}

} // namespace detail

template <typename M, typename N, typename A> struct ContextPtrComparator {
  bool operator()(const std::shared_ptr<Context<M, N, A>> &LHS,
                  const std::shared_ptr<Context<M, N, A>> &RHS) const {
    return LHS->getId() < RHS->getId();
  }
};

template <typename M, typename N, typename A> class InterProceduralAnalysis {
public:
  using ContextType = Context<M, N, A>;
  using ContextPtr = std::shared_ptr<ContextType>;
  using CallSiteType = CallSite<M, N, A>;
  using WorkListType = std::set<ContextPtr, ContextPtrComparator<M, N, A>>;

  struct SchedulerOptions {
    bool EnableParallelContextScheduling = false;
    std::size_t ContextStepBudget = 64;
  };

  struct SchedulerStats {
    std::size_t contexts_created = 0;
    std::size_t contexts_reused = 0;
    std::size_t context_steps = 0;
    std::size_t context_batches = 0;
    std::size_t caller_wakeups = 0;
    std::size_t summary_publications = 0;
    std::size_t suppressed_unchanged_wakeups = 0;
    std::size_t stale_callsite_replays = 0;
    std::size_t current_callsite_replays_skipped = 0;
    std::size_t parallel_worker_tasks = 0;
    std::size_t max_ready_contexts = 0;
  };

  explicit InterProceduralAnalysis(bool Reverse) : Reverse(Reverse) {}
  virtual ~InterProceduralAnalysis() = default;

  virtual A boundaryValue(const M &EntryPoint) = 0;
  virtual A copy(const A &Src) = 0;
  virtual void doAnalysis() = 0;
  virtual A meet(const A &LHS, const A &RHS) = 0;
  virtual const ProgramRepresentation<M, N> &programRepresentation() const = 0;
  virtual A topValue() = 0;

  const std::set<CallSiteType> *getCallers(ContextPtr Target) const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    return ContextTransitions.getCallers(Target);
  }

  ContextPtr getContext(const M &Method, const A &Value) const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    return getContextUnlocked(Method, Value);
  }

  const std::vector<ContextPtr> &getContexts(const M &Method) const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    auto It = Contexts.find(Method);
    if (It == Contexts.end()) {
      static const std::vector<ContextPtr> Empty;
      return Empty;
    }
    return It->second;
  }

  const ContextTransitionTable<M, N, A> &getContextTransitionTable() const {
    return ContextTransitions;
  }

  DataFlowSolution<N, A> getMeetOverValidPathsSolution() const {
    if (FreeResultsOnTheFly) {
      throw std::logic_error(
          "VASCO meet-over-valid-paths solution is unavailable when "
          "FreeResultsOnTheFly is enabled");
    }

    std::map<N, A> InValues;
    std::map<N, A> OutValues;

    for (const auto &MethodEntry : Contexts) {
      const auto Graph =
          programRepresentation().getControlFlowGraph(MethodEntry.first);
      if (!Graph) {
        continue;
      }

      for (const auto &Node : Graph->nodes()) {
        A In = topValueForConst();
        A Out = topValueForConst();
        bool HasValue = false;

        for (const auto &Context : MethodEntry.second) {
          if (Context->isFreed()) {
            throw std::logic_error("VASCO meet-over-valid-paths solution "
                                   "encountered freed context "
                                   "state");
          }
          In = meetConst(In, Context->getValueBefore(Node));
          Out = meetConst(Out, Context->getValueAfter(Node));
          HasValue = true;
        }

        if (HasValue) {
          InValues[Node] = In;
          OutValues[Node] = Out;
        }
      }
    }

    return DataFlowSolution<N, A>(std::move(InValues), std::move(OutValues));
  }

  std::set<M> getMethods() const {
    std::set<M> Methods;
    for (const auto &Entry : Contexts) {
      Methods.insert(Entry.first);
    }
    return Methods;
  }

  const std::map<M, ContextPtr> *
  getTargets(const CallSiteType &CallSite) const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    return ContextTransitions.getTargets(CallSite);
  }

  const SchedulerOptions &getSchedulerOptions() const {
    return SchedulingOptions;
  }

  void setParallelContextScheduling(bool Enable) {
    SchedulingOptions.EnableParallelContextScheduling = Enable;
  }

  void setContextStepBudget(std::size_t Budget) {
    SchedulingOptions.ContextStepBudget = Budget == 0 ? 1 : Budget;
  }

  const SchedulerStats &getSchedulerStats() const { return Stats; }

  void setFreeResultsOnTheFly(bool Enable) { FreeResultsOnTheFly = Enable; }
  void setVerbose(bool Enable) { Verbose = Enable; }
  bool getFreeResultsOnTheFly() const { return FreeResultsOnTheFly; }
  bool isVerbose() const { return Verbose; }

protected:
  ContextPtr registerContext(ContextPtr Context) {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    Contexts[Context->getMethod()].push_back(Context);
    ++Stats.contexts_created;
    return Context;
  }

  void enqueueContext(ContextPtr Context) {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    WorkList.insert(std::move(Context));
    Stats.max_ready_contexts =
        std::max(Stats.max_ready_contexts, WorkList.size());
  }

  ContextPtr newestContext() const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    assert(!WorkList.empty() && "newestContext() called with empty worklist");
    return *WorkList.rbegin();
  }

  bool hasPendingContexts() const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    return !WorkList.empty();
  }

  void removeContextFromWorklist(ContextPtr Context) {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    WorkList.erase(Context);
  }

  void wakeCallers(ContextPtr CurrentContext) {
    std::vector<CallSiteType> CallersSnapshot;
    {
      std::lock_guard<std::recursive_mutex> Lock(StateMutex);
      const auto *Callers = ContextTransitions.getCallers(CurrentContext);
      if (!Callers) {
        return;
      }
      CallersSnapshot.assign(Callers->begin(), Callers->end());
    }

    for (const auto &CallSite : CallersSnapshot) {
      auto CallingContext = CallSite.getCallingContext();
      {
        std::lock_guard<std::recursive_mutex> ContextLock(
            CallingContext->mutex());
        CallingContext->getWorkList().insert(CallSite.getCallNode());
      }
      enqueueContext(CallingContext);
      recordCallerWakeup();
    }
  }

  void wakeStaleCallers(ContextPtr CurrentContext) {
    std::vector<CallSiteType> CallersSnapshot;
    const std::size_t CurrentVersion = CurrentContext->getSummaryVersion();
    {
      std::lock_guard<std::recursive_mutex> Lock(StateMutex);
      const auto *Callers = ContextTransitions.getCallers(CurrentContext);
      if (!Callers) {
        return;
      }
      CallersSnapshot.assign(Callers->begin(), Callers->end());
    }

    for (const auto &CallSite : CallersSnapshot) {
      if (!isCallSiteStale(CallSite, CurrentContext, CurrentVersion)) {
        recordCurrentCallsiteReplaySkipped();
        continue;
      }

      auto CallingContext = CallSite.getCallingContext();
      {
        std::lock_guard<std::recursive_mutex> ContextLock(
            CallingContext->mutex());
        CallingContext->getWorkList().insert(CallSite.getCallNode());
      }
      enqueueContext(CallingContext);
      recordCallerWakeup();
      recordStaleCallsiteReplay();
    }
  }

  void freeReachableContextsIfDead(ContextPtr CurrentContext) {
    if (!FreeResultsOnTheFly) {
      return;
    }

    const auto ReachableContexts =
        ContextTransitions.reachableSet(CurrentContext, true);
    bool CanFree = true;
    for (const auto &Reachable : ReachableContexts) {
      if (WorkList.count(Reachable)) {
        CanFree = false;
        break;
      }
    }

    if (!CanFree) {
      return;
    }

    for (const auto &Reachable : ReachableContexts) {
      Reachable->freeMemory();
    }
  }

  void sanityCheckAnalysedContexts() const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    for (const auto &MethodEntry : Contexts) {
      for (const auto &Context : MethodEntry.second) {
        if (Context->isAnalysed()) {
          continue;
        }

        std::cerr << "*** ATTENTION ***: Only partial analysis of X"
                  << Context->getId();
        if constexpr (detail::IsOstreamWritable<M>::value) {
          std::cerr << ' ';
          detail::appendIfStreamable(std::cerr, Context->getMethod());
        }
        std::cerr << '\n';
      }
    }
  }

  A meetConst(const A &LHS, const A &RHS) const {
    return const_cast<InterProceduralAnalysis *>(this)->meet(LHS, RHS);
  }

  A topValueForConst() const {
    return const_cast<InterProceduralAnalysis *>(this)->topValue();
  }

  ContextPtr getContextUnlocked(const M &Method, const A &Value) const {
    auto It = Contexts.find(Method);
    if (It == Contexts.end()) {
      return nullptr;
    }

    for (const auto &Context : It->second) {
      if (Reverse) {
        if (Value == Context->getExitValue()) {
          return Context;
        }
      } else {
        if (Value == Context->getEntryValue()) {
          return Context;
        }
      }
    }

    return nullptr;
  }

  void addTransition(const CallSiteType &CallSite, ContextPtr TargetContext) {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ContextTransitions.addTransition(CallSite, std::move(TargetContext));
  }

  void observeSummaryVersion(const CallSiteType &CallSite,
                             ContextPtr TargetContext) {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ObservedSummaryVersions[ObservedSummaryKey{CallSite, TargetContext}] =
        TargetContext->getSummaryVersion();
  }

  void recordContextStep() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.context_steps;
  }

  void recordContextBatch() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.context_batches;
  }

  void recordContextReuse() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.contexts_reused;
  }

  void recordParallelWorkerTask() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.parallel_worker_tasks;
  }

  void recordCallerWakeup() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.caller_wakeups;
  }

  void recordSummaryPublication() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.summary_publications;
  }

  void recordSuppressedUnchangedWakeup() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.suppressed_unchanged_wakeups;
  }

  void recordStaleCallsiteReplay() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.stale_callsite_replays;
  }

  void recordCurrentCallsiteReplaySkipped() {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    ++Stats.current_callsite_replays_skipped;
  }

  ContextPtr takeRunnableContext(
      std::set<ContextPtr, ContextPtrComparator<M, N, A>> &RunningContexts) {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    for (auto It = WorkList.rbegin(); It != WorkList.rend(); ++It) {
      if (RunningContexts.count(*It) != 0) {
        continue;
      }
      ContextPtr Context = *It;
      WorkList.erase(std::next(It).base());
      RunningContexts.insert(Context);
      return Context;
    }
    return nullptr;
  }

  void finishRunnableContext(
      ContextPtr Context, bool HasMoreLocalWork,
      std::set<ContextPtr, ContextPtrComparator<M, N, A>> &RunningContexts) {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    RunningContexts.erase(Context);
    if (HasMoreLocalWork && !Context->isFreed()) {
      WorkList.insert(std::move(Context));
      Stats.max_ready_contexts =
          std::max(Stats.max_ready_contexts, WorkList.size());
    }
  }

  bool
  parallelWorkComplete(const std::set<ContextPtr, ContextPtrComparator<M, N, A>>
                           &RunningContexts) const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    return WorkList.empty() && RunningContexts.empty();
  }

  SchedulerOptions SchedulingOptions;
  SchedulerStats Stats;
  mutable std::recursive_mutex StateMutex;

  struct ObservedSummaryKey {
    CallSiteType CallSite;
    ContextPtr TargetContext;

    bool operator<(const ObservedSummaryKey &Other) const {
      if (CallSite < Other.CallSite) {
        return true;
      }
      if (Other.CallSite < CallSite) {
        return false;
      }
      const std::size_t ThisTargetId =
          TargetContext ? TargetContext->getId() : 0;
      const std::size_t OtherTargetId =
          Other.TargetContext ? Other.TargetContext->getId() : 0;
      return ThisTargetId < OtherTargetId;
    }
  };

  bool isCallSiteStale(const CallSiteType &CallSite, ContextPtr TargetContext,
                       std::size_t CurrentVersion) const {
    std::lock_guard<std::recursive_mutex> Lock(StateMutex);
    auto It = ObservedSummaryVersions.find(
        ObservedSummaryKey{CallSite, TargetContext});
    if (It == ObservedSummaryVersions.end()) {
      return true;
    }
    return It->second < CurrentVersion;
  }

  std::map<ObservedSummaryKey, std::size_t> ObservedSummaryVersions;
  WorkListType WorkList;
  std::map<M, std::vector<ContextPtr>> Contexts;
  ContextTransitionTable<M, N, A> ContextTransitions;
  bool Reverse = false;
  bool FreeResultsOnTheFly = false;
  bool Verbose = false;
};

} // namespace vasco
