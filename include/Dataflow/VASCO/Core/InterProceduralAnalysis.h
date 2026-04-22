#pragma once

#include "Dataflow/VASCO/Core/ContextTransitionTable.h"
#include "Dataflow/VASCO/Core/DataFlowSolution.h"
#include "Dataflow/VASCO/Core/ProgramRepresentation.h"

#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace vasco {

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

  explicit InterProceduralAnalysis(bool Reverse) : Reverse(Reverse) {}
  virtual ~InterProceduralAnalysis() = default;

  virtual A boundaryValue(const M &EntryPoint) = 0;
  virtual A copy(const A &Src) = 0;
  virtual void doAnalysis() = 0;
  virtual A meet(const A &LHS, const A &RHS) = 0;
  virtual const ProgramRepresentation<M, N> &programRepresentation() const = 0;
  virtual A topValue() = 0;

  const std::set<CallSiteType> *getCallers(ContextPtr Target) const {
    return ContextTransitions.getCallers(Target);
  }

  ContextPtr getContext(const M &Method, const A &Value) const {
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

  const std::vector<ContextPtr> &getContexts(const M &Method) const {
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
    std::map<N, A> InValues;
    std::map<N, A> OutValues;

    for (const auto &MethodEntry : Contexts) {
      const auto Graph = programRepresentation().getControlFlowGraph(MethodEntry.first);
      if (!Graph) {
        continue;
      }

      for (const auto &Node : Graph->nodes()) {
        A In = topValueForConst();
        A Out = topValueForConst();
        bool HasValue = false;

        for (const auto &Context : MethodEntry.second) {
          if (Context->isFreed()) {
            continue;
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

  const std::map<M, ContextPtr> *getTargets(const CallSiteType &CallSite) const {
    return ContextTransitions.getTargets(CallSite);
  }

  void setFreeResultsOnTheFly(bool Enable) { FreeResultsOnTheFly = Enable; }
  void setVerbose(bool Enable) { Verbose = Enable; }
  bool getFreeResultsOnTheFly() const { return FreeResultsOnTheFly; }
  bool isVerbose() const { return Verbose; }

protected:
  ContextPtr registerContext(ContextPtr Context) {
    Contexts[Context->getMethod()].push_back(Context);
    return Context;
  }

  void enqueueContext(ContextPtr Context) { WorkList.insert(std::move(Context)); }

  ContextPtr newestContext() const {
    assert(!WorkList.empty() && "newestContext() called with empty worklist");
    return *WorkList.rbegin();
  }

  void removeContextFromWorklist(ContextPtr Context) { WorkList.erase(Context); }

  void wakeCallers(ContextPtr CurrentContext) {
    const auto *Callers = ContextTransitions.getCallers(CurrentContext);
    if (!Callers) {
      return;
    }

    for (const auto &CallSite : *Callers) {
      auto CallingContext = CallSite.getCallingContext();
      CallingContext->getWorkList().insert(CallSite.getCallNode());
      WorkList.insert(CallingContext);
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
    for (const auto &MethodEntry : Contexts) {
      for (const auto &Context : MethodEntry.second) {
        (void)Context;
      }
    }
  }

  A meetConst(const A &LHS, const A &RHS) const {
    return const_cast<InterProceduralAnalysis *>(this)->meet(LHS, RHS);
  }

  A topValueForConst() const {
    return const_cast<InterProceduralAnalysis *>(this)->topValue();
  }

  WorkListType WorkList;
  std::map<M, std::vector<ContextPtr>> Contexts;
  ContextTransitionTable<M, N, A> ContextTransitions;
  bool Reverse = false;
  bool FreeResultsOnTheFly = false;
  bool Verbose = false;
};

} // namespace vasco
