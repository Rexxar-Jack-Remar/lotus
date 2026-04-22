#pragma once

#include "Dataflow/VASCO/Analyses/ForwardInterProceduralAnalysis.h"
#include "Dataflow/VASCO/Core/ProgramRepresentation.h"
#include "Dataflow/VASCO/Support/LLVMAnalysisTypes.h"

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

namespace vasco {
namespace llvmir {

class CopyConstantAnalysis
    : public ForwardInterProceduralAnalysis<llvm::Function *, llvm::Instruction *,
                                            FlowMap<const llvm::Constant *>> {
public:
  using MethodType = llvm::Function *;
  using NodeType = llvm::Instruction *;
  using DomainType = FlowMap<const llvm::Constant *>;
  using ContextPtr =
      std::shared_ptr<Context<MethodType, NodeType, DomainType>>;

  explicit CopyConstantAnalysis(
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
      } else if (It->second != nullptr && It->second != Entry.second) {
        It->second = nullptr;
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
      if (!Phi->getType()->isVoidTy()) {
        OutValue[ValueKey::forValue(Phi)] = evaluatePhi(Phi, InValue);
      }
      return OutValue;
    }

    if (!Node->getType()->isVoidTy()) {
      OutValue[ValueKey::forValue(Node)] = evaluateInstruction(Node, InValue);
    }

    if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Node)) {
      if (auto *RetVal = Ret->getReturnValue()) {
        OutValue[ValueKey::returnValue()] = constantOf(RetVal, InValue);
      }
    }

    return OutValue;
  }

  DomainType callEntryFlowFunction(ContextPtr, const MethodType &CalledMethod,
                                   const NodeType &Node,
                                   const DomainType &InValue) override {
    DomainType EntryValue = topValue();
    auto *Call = llvm::cast<llvm::CallBase>(Node);

    for (unsigned I = 0, E = Call->arg_size(); I < E; ++I) {
      if (I >= CalledMethod->arg_size()) {
        break;
      }
      auto *ArgIt = CalledMethod->arg_begin();
      std::advance(ArgIt, I);
      EntryValue[ValueKey::forValue(&*ArgIt)] =
          constantOf(Call->getArgOperand(I), InValue);
    }

    return EntryValue;
  }

  DomainType callExitFlowFunction(ContextPtr, const MethodType &,
                                  const NodeType &Node,
                                  const DomainType &ExitValue) override {
    DomainType AfterCallValue = topValue();
    if (!Node->getType()->isVoidTy()) {
      auto It = ExitValue.find(ValueKey::returnValue());
      if (It != ExitValue.end()) {
        AfterCallValue[ValueKey::forValue(Node)] = It->second;
      }
    }
    return AfterCallValue;
  }

  DomainType callLocalFlowFunction(ContextPtr, const NodeType &Node,
                                   const DomainType &InValue) override {
    DomainType AfterCallValue = copy(InValue);
    if (!Node->getType()->isVoidTy()) {
      AfterCallValue.erase(ValueKey::forValue(Node));
    }
    return AfterCallValue;
  }

  DomainType unknownCallFlowFunction(ContextPtr, const NodeType &Node,
                                     const DomainType &InValue) override {
    DomainType AfterCallValue = copy(InValue);
    if (!Node->getType()->isVoidTy()) {
      AfterCallValue[ValueKey::forValue(Node)] = nullptr;
    }
    return AfterCallValue;
  }

private:
  const llvm::Constant *constantOf(const llvm::Value *Value,
                                   const DomainType &State) const {
    if (Value == nullptr) {
      return nullptr;
    }

    Value = stripCasts(Value);

    if (auto *Constant = llvm::dyn_cast<llvm::Constant>(Value)) {
      return Constant;
    }

    auto It = State.find(ValueKey::forValue(Value));
    if (It != State.end()) {
      return It->second;
    }

    return nullptr;
  }

  const llvm::Constant *evaluatePhi(const llvm::PHINode *Phi,
                                    const DomainType &State) const {
    const llvm::Constant *Result = nullptr;
    bool Initialized = false;

    for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
      const llvm::Constant *Incoming = constantOf(Phi->getIncomingValue(I), State);
      if (!Initialized) {
        Result = Incoming;
        Initialized = true;
        continue;
      }
      if (Result != Incoming) {
        return nullptr;
      }
    }

    return Result;
  }

  const llvm::Constant *evaluateInstruction(const NodeType &Node,
                                            const DomainType &State) const {
    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Node)) {
      return constantOf(Cast->getOperand(0), State);
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(Node)) {
      const llvm::Constant *TrueValue = constantOf(Select->getTrueValue(), State);
      const llvm::Constant *FalseValue = constantOf(Select->getFalseValue(), State);
      return TrueValue == FalseValue ? TrueValue : nullptr;
    }

    return nullptr;
  }

  const ProgramRepresentation<MethodType, NodeType> &Program;
};

} // namespace llvmir
} // namespace vasco
