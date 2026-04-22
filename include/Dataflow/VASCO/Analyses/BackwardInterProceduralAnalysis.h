#pragma once

#include "Dataflow/VASCO/Core/InterProceduralAnalysis.h"

#include <optional>

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

    while (!this->WorkList.empty()) {
      auto CurrentContext = this->newestContext();

      if (CurrentContext->getWorkList().empty()) {
        this->removeContextFromWorklist(CurrentContext);
        continue;
      }

      auto ItemIt = CurrentContext->getWorkList().begin();
      const auto Item = *ItemIt;
      CurrentContext->getWorkList().erase(ItemIt);

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
          const auto MaybeTargets =
              this->programRepresentation().resolveTargets(CurrentContext->getMethod(),
                                                          Node);
          if (!MaybeTargets.has_value()) {
            this->ContextTransitions.addTransition(CallSiteType(CurrentContext, Node),
                                                   nullptr);
            In = unknownCallFlowFunction(CurrentContext, Node, Out);
          } else if (MaybeTargets->empty()) {
            In = callLocalFlowFunction(CurrentContext, Node, Out);
          } else {
            for (const auto &TargetMethod : *MaybeTargets) {
              const A ExitValue =
                  callExitFlowFunction(CurrentContext, TargetMethod, Node, Out);
              CallSiteType CallSite(CurrentContext, Node);

              auto TargetContext = this->getContext(TargetMethod, ExitValue);
              if (!TargetContext) {
                TargetContext = initContext(TargetMethod, ExitValue);
              }

              this->ContextTransitions.addTransition(CallSite, TargetContext);

              if (TargetContext->isAnalysed()) {
                Hit = true;
                const A EntryValue = TargetContext->getEntryValue();
                const A CallValue = callEntryFlowFunction(CurrentContext,
                                                          TargetMethod, Node,
                                                          EntryValue);
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

        CurrentContext->setEntryValue(EntryValue);
        CurrentContext->markAnalysed();
        this->wakeCallers(CurrentContext);
        this->freeReachableContextsIfDead(CurrentContext);
      }
    }

    this->sanityCheckAnalysedContexts();
  }

protected:
  ContextPtr initContext(const M &Method, const A &ExitValue) {
    auto NewContext = std::make_shared<vasco::Context<M, N, A>>(
        Method, this->programRepresentation().getControlFlowGraph(Method), true);

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
