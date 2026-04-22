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

class SignAnalysis
    : public ForwardInterProceduralAnalysis<llvm::Function *, llvm::Instruction *,
                                            FlowMap<Sign>> {
public:
  using MethodType = llvm::Function *;
  using NodeType = llvm::Instruction *;
  using DomainType = FlowMap<Sign>;
  using ContextPtr =
      std::shared_ptr<Context<MethodType, NodeType, DomainType>>;

  explicit SignAnalysis(
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
        It->second = meetSigns(It->second, Entry.second);
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
      if (Phi->getType()->isIntegerTy()) {
        Sign Value = Sign::Top;
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
          Value = meetSigns(Value, signOf(Phi->getIncomingValue(I), InValue));
        }
        OutValue[ValueKey::forValue(Phi)] = Value;
      }
      return OutValue;
    }

    if (Node->getType()->isIntegerTy() && !Node->getType()->isVoidTy()) {
      OutValue[ValueKey::forValue(Node)] = evaluateInstruction(Node, InValue);
    }

    if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Node)) {
      if (auto *RetVal = Ret->getReturnValue()) {
        OutValue[ValueKey::returnValue()] = signOf(RetVal, InValue);
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
      auto ArgIt = CalledMethod->arg_begin();
      std::advance(ArgIt, I);
      EntryValue[ValueKey::forValue(&*ArgIt)] =
          signOf(Call->getArgOperand(I), InValue);
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

private:
  Sign signOf(const llvm::Value *Value, const DomainType &State) const {
    if (Value == nullptr) {
      return Sign::Bottom;
    }

    Value = stripCasts(Value);

    if (auto *Int = llvm::dyn_cast<llvm::ConstantInt>(Value)) {
      if (Int->isNegative()) {
        return Sign::Negative;
      }
      if (Int->isZero()) {
        return Sign::Zero;
      }
      return Sign::Positive;
    }

    auto It = State.find(ValueKey::forValue(Value));
    if (It != State.end()) {
      return It->second;
    }

    return Sign::Top;
  }

  Sign evaluateInstruction(const NodeType &Node,
                           const DomainType &State) const {
    if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(Node)) {
      const Sign LHS = signOf(BinOp->getOperand(0), State);
      const Sign RHS = signOf(BinOp->getOperand(1), State);
      switch (BinOp->getOpcode()) {
      case llvm::Instruction::Add:
        return plusSigns(LHS, RHS);
      case llvm::Instruction::Mul:
        return multiplySigns(LHS, RHS);
      case llvm::Instruction::Sub:
        if (auto *Zero = llvm::dyn_cast<llvm::ConstantInt>(BinOp->getOperand(0))) {
          if (Zero->isZero()) {
            return negateSign(RHS);
          }
        }
        return Sign::Bottom;
      default:
        return Sign::Bottom;
      }
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Node)) {
      return signOf(Cast->getOperand(0), State);
    }

    return Sign::Bottom;
  }

  const ProgramRepresentation<MethodType, NodeType> &Program;
};

} // namespace llvmir
} // namespace vasco
