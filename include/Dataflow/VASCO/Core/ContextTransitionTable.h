#pragma once

#include "Dataflow/VASCO/Core/CallSite.h"

#include <map>
#include <memory>
#include <set>
#include <stack>

namespace vasco {

template <typename M, typename N, typename A> class ContextTransitionTable {
public:
  using ContextType = Context<M, N, A>;
  using ContextPtr = std::shared_ptr<ContextType>;
  using CallSiteType = CallSite<M, N, A>;

  void addTransition(const CallSiteType &CallSite, ContextPtr TargetContext) {
    if (TargetContext) {
      const auto &TargetMethod = TargetContext->getMethod();

      if (!Callers.count(TargetContext)) {
        Callers[TargetContext] = {};
      }

      if (Transitions.count(CallSite) &&
          Transitions[CallSite].count(TargetMethod)) {
        auto OldTarget = Transitions[CallSite][TargetMethod];
        Callers[OldTarget].erase(CallSite);
      }

      Transitions[CallSite][TargetMethod] = TargetContext;
      Callers[TargetContext].insert(CallSite);
      DefaultCallSites.erase(CallSite);
    } else {
      if (Transitions.count(CallSite)) {
        for (const auto &Entry : Transitions[CallSite]) {
          Callers[Entry.second].erase(CallSite);
        }
        Transitions.erase(CallSite);
      }
      DefaultCallSites.insert(CallSite);
    }

    auto Source = CallSite.getCallingContext();
    if (Source) {
      CallSitesOfContexts[Source].insert(CallSite);
    }
  }

  const std::map<ContextPtr, std::set<CallSiteType>> &getCallers() const {
    return Callers;
  }

  const std::set<CallSiteType> *getCallers(ContextPtr Target) const {
    auto It = Callers.find(Target);
    if (It == Callers.end()) {
      return nullptr;
    }
    return &It->second;
  }

  const std::map<ContextPtr, std::set<CallSiteType>> &
  getCallSitesOfContexts() const {
    return CallSitesOfContexts;
  }

  const std::set<CallSiteType> &getDefaultCallSites() const {
    return DefaultCallSites;
  }

  const std::map<M, ContextPtr> *getTargets(const CallSiteType &CallSite) const {
    auto It = Transitions.find(CallSite);
    if (It == Transitions.end()) {
      return nullptr;
    }
    return &It->second;
  }

  const std::map<CallSiteType, std::map<M, ContextPtr>> &getTransitions() const {
    return Transitions;
  }

  std::set<ContextPtr> reachableSet(ContextPtr Source, bool IgnoreFreed) const {
    std::set<ContextPtr> ReachableContexts;
    if (!Source) {
      return ReachableContexts;
    }

    std::stack<ContextPtr> Stack;
    Stack.push(Source);

    while (!Stack.empty()) {
      auto Current = Stack.top();
      Stack.pop();

      auto SitesIt = CallSitesOfContexts.find(Current);
      if (SitesIt == CallSitesOfContexts.end()) {
        continue;
      }

      for (const auto &Site : SitesIt->second) {
        if (DefaultCallSites.count(Site)) {
          continue;
        }

        auto TargetsIt = Transitions.find(Site);
        if (TargetsIt == Transitions.end()) {
          continue;
        }

        for (const auto &TargetEntry : TargetsIt->second) {
          auto Target = TargetEntry.second;
          if (!Target) {
            continue;
          }
          if (IgnoreFreed && Target->isFreed()) {
            continue;
          }
          if (ReachableContexts.insert(Target).second) {
            Stack.push(Target);
          }
        }
      }
    }

    return ReachableContexts;
  }

private:
  std::map<ContextPtr, std::set<CallSiteType>> Callers;
  std::map<CallSiteType, std::map<M, ContextPtr>> Transitions;
  std::map<ContextPtr, std::set<CallSiteType>> CallSitesOfContexts;
  std::set<CallSiteType> DefaultCallSites;
};

} // namespace vasco
