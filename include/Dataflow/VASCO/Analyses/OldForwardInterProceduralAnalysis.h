#pragma once

#include "Dataflow/VASCO/Core/InterProceduralAnalysis.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace vasco {

template <typename M, typename N, typename A>
class OldForwardInterProceduralAnalysis
    : public InterProceduralAnalysis<M, N, A> {
public:
  using Base = InterProceduralAnalysis<M, N, A>;
  using typename Base::CallSiteType;
  using typename Base::ContextPtr;

  OldForwardInterProceduralAnalysis() : Base(false) {}

  void doAnalysis() override {
    for (const auto &EntryPoint : this->programRepresentation().getEntryPoints()) {
      auto EntryContext = std::make_shared<vasco::Context<M, N, A>>(
          EntryPoint, this->programRepresentation().getControlFlowGraph(EntryPoint),
          false);
      initContext(EntryContext, this->boundaryValue(EntryPoint));
    }

    while (!AnalysisStack.empty()) {
      auto CurrentContext = AnalysisStack.back();

      if (!CurrentContext->getWorkList().empty()) {
        auto ItemIt = CurrentContext->getWorkList().begin();
        const auto Item = *ItemIt;
        CurrentContext->getWorkList().erase(ItemIt);

        if (Item.has_value()) {
          const auto &Node = *Item;
          const auto Graph = CurrentContext->getControlFlowGraph();

          const auto Predecessors = Graph->predsOf(Node);
          if (!Predecessors.empty()) {
            auto PredIt = Predecessors.begin();
            A In = CurrentContext->getValueAfter(*PredIt++);
            while (PredIt != Predecessors.end()) {
              In = this->meet(In, CurrentContext->getValueAfter(*PredIt++));
            }
            CurrentContext->setValueBefore(Node, In);
          }

          const A PrevOut = CurrentContext->getValueAfter(Node);
          const A In = CurrentContext->getValueBefore(Node);

          auto MaybeOut = flowFunction(CurrentContext, Node, In);
          A Out = MaybeOut.has_value() ? *MaybeOut : PrevOut;

          CurrentContext->setValueAfter(Node, Out);

          if (!(Out == PrevOut)) {
            for (const auto &Successor : Graph->succsOf(Node)) {
              CurrentContext->getWorkList().insert(Successor);
            }
            for (const auto &Tail : Graph->tails()) {
              if (Tail == Node) {
                CurrentContext->getWorkList().insert(std::nullopt);
                break;
              }
            }
          }
        } else {
          const auto Graph = CurrentContext->getControlFlowGraph();
          A ExitFlow = this->topValue();
          for (const auto &Tail : Graph->tails()) {
            ExitFlow = this->meet(ExitFlow, CurrentContext->getValueAfter(Tail));
          }
          CurrentContext->setExitValue(ExitFlow);
          CurrentContext->markAnalysed();

          const auto *Callers = this->ContextTransitions.getCallers(CurrentContext);
          if (Callers) {
            std::vector<CallSiteType> SortedCallers(Callers->begin(), Callers->end());
            std::sort(SortedCallers.begin(), SortedCallers.end());
            for (const auto &CallSite : SortedCallers) {
              auto CallingContext = CallSite.getCallingContext();
              CallingContext->getWorkList().insert(CallSite.getCallNode());
              if (!stackContains(CallingContext)) {
                AnalysisStack.push_back(CallingContext);
              }
            }
          }

          if (this->getFreeResultsOnTheFly()) {
            const auto ReachableContexts =
                this->ContextTransitions.reachableSet(CurrentContext, true);
            bool CanFree = true;
            for (const auto &ReachableContext : ReachableContexts) {
              if (stackContains(ReachableContext)) {
                CanFree = false;
                break;
              }
            }
            if (CanFree) {
              for (const auto &ReachableContext : ReachableContexts) {
                ReachableContext->freeMemory();
              }
            }
          }
        }
      } else {
        AnalysisStack.pop_back();
      }
    }

    this->sanityCheckAnalysedContexts();
  }

protected:
  void initContext(ContextPtr Context, const A &EntryValue) {
    const auto &Method = Context->getMethod();
    const auto Graph = Context->getControlFlowGraph();

    for (const auto &Node : Graph->nodes()) {
      Context->setValueBefore(Node, this->topValue());
      Context->setValueAfter(Node, this->topValue());
    }

    Context->setEntryValue(this->copy(EntryValue));
    for (const auto &Head : Graph->heads()) {
      Context->setValueBefore(Head, this->copy(EntryValue));
      Context->getWorkList().insert(Head);
    }

    this->registerContext(Context);
    AnalysisStack.push_back(Context);
  }

  std::optional<A> processCall(ContextPtr CallerContext, const N &CallNode,
                               const M &Method, const A &EntryValue) {
    CallSiteType CallSite(CallerContext, CallNode);

    auto CalleeContext = this->getContext(Method, EntryValue);
    if (!CalleeContext) {
      CalleeContext = std::make_shared<vasco::Context<M, N, A>>(
          Method, this->programRepresentation().getControlFlowGraph(Method), false);
      initContext(CalleeContext, EntryValue);
    }

    this->ContextTransitions.addTransition(CallSite, CalleeContext);

    if (CalleeContext->isAnalysed()) {
      return CalleeContext->getExitValue();
    }
    return std::nullopt;
  }

  virtual std::optional<A> flowFunction(ContextPtr Context, const N &Node,
                                        const A &InValue) = 0;

private:
  bool stackContains(ContextPtr Context) const {
    return std::find(AnalysisStack.begin(), AnalysisStack.end(), Context) !=
           AnalysisStack.end();
  }

  std::vector<ContextPtr> AnalysisStack;
};

} // namespace vasco
