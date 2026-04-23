#pragma once

#include "Dataflow/VASCO/Solver/ForwardInterProceduralAnalysis.h"
#include "Dataflow/VASCO/Core/ProgramRepresentation.h"
#include "Dataflow/VASCO/Support/LLVMAnalysisTypes.h"

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

namespace vasco {
namespace llvmir {

enum class Nullness {
  Top,
  Null,
  NonNull,
  MaybeNull,
};

inline Nullness meetNullness(Nullness LHS, Nullness RHS) {
  if (LHS == RHS) {
    return LHS;
  }
  if (LHS == Nullness::Top) {
    return RHS;
  }
  if (RHS == Nullness::Top) {
    return LHS;
  }
  return Nullness::MaybeNull;
}

class NullnessAnalysis
    : public ForwardInterProceduralAnalysis<
          llvm::Function *, llvm::Instruction *, FlowMap<Nullness>> {
public:
  using MethodType = llvm::Function *;
  using NodeType = llvm::Instruction *;
  using DomainType = FlowMap<Nullness>;
  using ContextPtr = std::shared_ptr<Context<MethodType, NodeType, DomainType>>;

  explicit NullnessAnalysis(
      const ProgramRepresentation<MethodType, NodeType> &Program)
      : Program(Program) {}

  DomainType boundaryValue(const MethodType &) override { return topValue(); }
  DomainType copy(const DomainType &Src) override { return Src; }

  DomainType meet(const DomainType &LHS, const DomainType &RHS) override {
    DomainType Result = LHS;
    for (const auto &Entry : RHS) {
      auto It = Result.find(Entry.first);
      if (It == Result.end()) {
        Result.emplace(Entry.first, Entry.second);
      } else {
        It->second = meetNullness(It->second, Entry.second);
      }
    }
    return Result;
  }

  const ProgramRepresentation<MethodType, NodeType> &
  programRepresentation() const override {
    return Program;
  }

  DomainType topValue() override { return {}; }

protected:
  DomainType normalFlowFunction(ContextPtr, const NodeType &Node,
                                const DomainType &InValue) override {
    DomainType OutValue = copy(InValue);

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Node)) {
      if (Phi->getType()->isPointerTy()) {
        Nullness Value = Nullness::Top;
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
          Value = meetNullness(Value,
                               nullnessOf(Phi->getIncomingValue(I), InValue));
        }
        OutValue[ValueKey::forValue(Phi)] = Value;
      }
      return OutValue;
    }

    if (Node->getType()->isPointerTy() && !Node->getType()->isVoidTy()) {
      OutValue[ValueKey::forValue(Node)] = evaluateInstruction(Node, InValue);
    }

    if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Node)) {
      if (auto *RetVal = Ret->getReturnValue();
          RetVal && RetVal->getType()->isPointerTy()) {
        OutValue[ValueKey::returnValue()] = nullnessOf(RetVal, InValue);
      }
    }

    return OutValue;
  }

  DomainType callEntryFlowFunction(ContextPtr, const MethodType &CalledMethod,
                                   const NodeType &Node,
                                   const DomainType &InValue) override {
    DomainType EntryValue = topValue();
    auto *Call = llvm::cast<llvm::CallBase>(Node);

    auto *FormalIt = CalledMethod->arg_begin();
    for (unsigned I = 0, E = Call->arg_size();
         I < E && FormalIt != CalledMethod->arg_end(); ++I, ++FormalIt) {
      if (!FormalIt->getType()->isPointerTy()) {
        continue;
      }
      EntryValue[ValueKey::forValue(&*FormalIt)] =
          nullnessOf(Call->getArgOperand(I), InValue);
    }

    return EntryValue;
  }

  DomainType callExitFlowFunction(ContextPtr, const MethodType &,
                                  const NodeType &Node,
                                  const DomainType &ExitValue) override {
    DomainType AfterCallValue = topValue();
    if (!Node->getType()->isPointerTy() || Node->getType()->isVoidTy()) {
      return AfterCallValue;
    }

    auto It = ExitValue.find(ValueKey::returnValue());
    if (It != ExitValue.end()) {
      AfterCallValue[ValueKey::forValue(Node)] = It->second;
    }
    return AfterCallValue;
  }

  DomainType callLocalFlowFunction(ContextPtr, const NodeType &Node,
                                   const DomainType &InValue) override {
    DomainType AfterCallValue = copy(InValue);
    if (Node->getType()->isPointerTy() && !Node->getType()->isVoidTy()) {
      AfterCallValue.erase(ValueKey::forValue(Node));
    }
    return AfterCallValue;
  }

  DomainType unknownCallFlowFunction(ContextPtr, const NodeType &Node,
                                     const DomainType &InValue) override {
    DomainType AfterCallValue = copy(InValue);
    if (Node->getType()->isPointerTy() && !Node->getType()->isVoidTy()) {
      AfterCallValue[ValueKey::forValue(Node)] = Nullness::MaybeNull;
    }
    return AfterCallValue;
  }

private:
  Nullness nullnessOf(const llvm::Value *Value, const DomainType &State) const {
    if (Value == nullptr) {
      return Nullness::MaybeNull;
    }

    Value = stripCasts(Value);

    if (llvm::isa<llvm::ConstantPointerNull>(Value)) {
      return Nullness::Null;
    }

    if (llvm::isa<llvm::AllocaInst>(Value) ||
        llvm::isa<llvm::GlobalValue>(Value)) {
      return Nullness::NonNull;
    }

    auto It = State.find(ValueKey::forValue(Value));
    if (It != State.end()) {
      return It->second;
    }

    return Nullness::Top;
  }

  Nullness evaluateInstruction(const NodeType &Node,
                               const DomainType &State) const {
    if (llvm::isa<llvm::AllocaInst>(Node)) {
      return Nullness::NonNull;
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Node)) {
      return nullnessOf(Cast->getOperand(0), State);
    }

    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Node)) {
      return nullnessOf(GEP->getPointerOperand(), State);
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(Node)) {
      const Nullness TrueValue = nullnessOf(Select->getTrueValue(), State);
      const Nullness FalseValue = nullnessOf(Select->getFalseValue(), State);
      return meetNullness(TrueValue, FalseValue);
    }

    if (llvm::isa<llvm::LoadInst>(Node)) {
      return Nullness::MaybeNull;
    }

    return Nullness::Top;
  }

  const ProgramRepresentation<MethodType, NodeType> &Program;
};

} // namespace llvmir
} // namespace vasco
