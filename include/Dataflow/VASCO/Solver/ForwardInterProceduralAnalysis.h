#pragma once

#include "Dataflow/VASCO/Core/InterProceduralAnalysis.h"

#include <optional>

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

    while (!this->WorkList.empty()) {
      auto CurrentContext = this->newestContext();

      if (CurrentContext->getWorkList().empty()) {
        CurrentContext->markAnalysed();
        this->removeContextFromWorklist(CurrentContext);
        continue;
      }

      auto ItemIt = CurrentContext->getWorkList().begin();
      const auto Item = *ItemIt;
      CurrentContext->getWorkList().erase(ItemIt);

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
          const auto MaybeTargets =
              this->programRepresentation().resolveTargets(
                  CurrentContext->getMethod(), Node);
          if (!MaybeTargets.has_value()) {
            this->ContextTransitions.addTransition(
                CallSiteType(CurrentContext, Node), nullptr);
            Out = unknownCallFlowFunction(CurrentContext, Node, In);
          } else if (!MaybeTargets->empty()) {
            for (const auto &TargetMethod : *MaybeTargets) {
              const A EntryValue =
                  callEntryFlowFunction(CurrentContext, TargetMethod, Node, In);
              CallSiteType CallSite(CurrentContext, Node);

              auto TargetContext = this->getContext(TargetMethod, EntryValue);
              if (!TargetContext) {
                TargetContext = initContext(TargetMethod, EntryValue);
              }

              this->ContextTransitions.addTransition(CallSite, TargetContext);

              if (TargetContext->isAnalysed()) {
                Hit = true;
                const A ExitValue = TargetContext->getExitValue();
                const A ReturnedValue = callExitFlowFunction(
                    CurrentContext, TargetMethod, Node, ExitValue);
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

        CurrentContext->setExitValue(ExitValue);
        CurrentContext->markAnalysed();
        this->wakeCallers(CurrentContext);
        this->freeReachableContextsIfDead(CurrentContext);
      }
    }

    this->sanityCheckAnalysedContexts();
  }

protected:
  ContextPtr initContextForPhantomMethod(const M &Method, const A &EntryValue) {
    auto NewContext = std::make_shared<vasco::Context<M, N, A>>(Method);
    NewContext->setEntryValue(this->copy(EntryValue));
    NewContext->setExitValue(this->copy(EntryValue));
    NewContext->markAnalysed();
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
