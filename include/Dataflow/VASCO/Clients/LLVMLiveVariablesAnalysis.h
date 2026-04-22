#pragma once

#include "Dataflow/VASCO/Analyses/BackwardInterProceduralAnalysis.h"
#include "Dataflow/VASCO/Core/ProgramRepresentation.h"
#include "Dataflow/VASCO/Support/LLVMAnalysisTypes.h"

#include <llvm/IR/Argument.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

#include <set>

namespace vasco {
namespace llvmir {

class LiveVariablesAnalysis
    : public BackwardInterProceduralAnalysis<llvm::Function *, llvm::Instruction *,
                                             std::set<ValueKey>> {
public:
  using MethodType = llvm::Function *;
  using NodeType = llvm::Instruction *;
  using DomainType = std::set<ValueKey>;
  using ContextPtr =
      std::shared_ptr<Context<MethodType, NodeType, DomainType>>;

  explicit LiveVariablesAnalysis(
      const ProgramRepresentation<MethodType, NodeType> &Program)
      : Program(Program) {}

  DomainType boundaryValue(const MethodType &Method) override {
    DomainType Boundary = topValue();
    if (Method != nullptr && !Method->getReturnType()->isVoidTy()) {
      Boundary.insert(ValueKey::returnValue());
    }
    return Boundary;
  }
  DomainType copy(const DomainType &Src) override { return Src; }

  DomainType meet(const DomainType &LHS, const DomainType &RHS) override {
    DomainType Result = LHS;
    Result.insert(RHS.begin(), RHS.end());
    return Result;
  }

  const ProgramRepresentation<MethodType, NodeType> &
  programRepresentation() const override {
    return Program;
  }

  DomainType topValue() override { return {}; }

protected:
  DomainType normalFlowFunction(ContextPtr, const NodeType &Node,
                                const DomainType &OutValue) override {
    DomainType InValue = copy(OutValue);

    if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Node)) {
      if (InValue.erase(ValueKey::returnValue()) != 0) {
        addTrackedValue(Ret->getReturnValue(), InValue);
      }
      return InValue;
    }

    killDefinition(Node, InValue);
    addInstructionUses(Node, InValue);
    return InValue;
  }

  DomainType callEntryFlowFunction(ContextPtr, const MethodType &CalledMethod,
                                   const NodeType &Node,
                                   const DomainType &EntryValue) override {
    DomainType InValue = topValue();
    auto *Call = llvm::cast<llvm::CallBase>(Node);

    auto *FormalIt = CalledMethod->arg_begin();
    for (unsigned I = 0, E = Call->arg_size();
         I < E && FormalIt != CalledMethod->arg_end(); ++I, ++FormalIt) {
      if (EntryValue.count(ValueKey::forValue(&*FormalIt)) == 0) {
        continue;
      }
      addTrackedValue(Call->getArgOperand(I), InValue);
    }

    return InValue;
  }

  DomainType callExitFlowFunction(ContextPtr, const MethodType &,
                                  const NodeType &Node,
                                  const DomainType &OutValue) override {
    DomainType InValue = topValue();

    if (!Node->getType()->isVoidTy() &&
        OutValue.count(ValueKey::forValue(Node)) != 0) {
      InValue.insert(ValueKey::returnValue());
    }

    return InValue;
  }

  DomainType callLocalFlowFunction(ContextPtr, const NodeType &Node,
                                   const DomainType &OutValue) override {
    DomainType InValue = copy(OutValue);
    killDefinition(Node, InValue);
    addInstructionUses(Node, InValue);
    return InValue;
  }

private:
  static bool isTrackedValue(const llvm::Value *Value) {
    return llvm::isa<llvm::Argument>(Value) ||
           llvm::isa<llvm::Instruction>(Value) ||
           llvm::isa<llvm::GlobalVariable>(Value);
  }

  void addTrackedValue(const llvm::Value *Value, DomainType &State) const {
    if (Value == nullptr) {
      return;
    }

    Value = stripCasts(Value);
    if (isTrackedValue(Value)) {
      State.insert(ValueKey::forValue(Value));
    }
  }

  void addInstructionUses(const llvm::Instruction *Inst,
                          DomainType &State) const {
    for (const llvm::Use &Use : Inst->operands()) {
      addTrackedValue(Use.get(), State);
    }
  }

  void killDefinition(const llvm::Instruction *Inst, DomainType &State) const {
    if (!Inst->getType()->isVoidTy()) {
      State.erase(ValueKey::forValue(Inst));
    }
  }

  const ProgramRepresentation<MethodType, NodeType> &Program;
};

} // namespace llvmir
} // namespace vasco
