#pragma once

#include "Dataflow/VASCO/Adapters/LLVM/DefaultLLVMProgramRepresentation.h"
#include "Dataflow/VASCO/Core/ProgramRepresentation.h"
#include "Dataflow/VASCO/Solver/OldForwardInterProceduralAnalysis.h"
#include "Dataflow/VASCO/Support/LLVMPointsToMemory.h"

#include <optional>
#include <vector>

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

namespace vasco {
namespace llvmir {

class PointsToAnalysis
    : public OldForwardInterProceduralAnalysis<
          llvm::Function *, llvm::Instruction *, PointsToGraph> {
public:
  using MethodType = llvm::Function *;
  using NodeType = llvm::Instruction *;
  using DomainType = PointsToGraph;
  using ContextPtr = std::shared_ptr<Context<MethodType, NodeType, DomainType>>;

  explicit PointsToAnalysis(
      const ProgramRepresentation<MethodType, NodeType> &Program);

  DomainType boundaryValue(const MethodType &EntryPoint) override;
  DomainType copy(const DomainType &Src) override { return Src; }
  DomainType meet(const DomainType &LHS, const DomainType &RHS) override;
  const ProgramRepresentation<MethodType, NodeType> &
  programRepresentation() const override {
    return Program;
  }
  DomainType topValue() override { return {}; }

protected:
  std::optional<DomainType> flowFunction(ContextPtr Context,
                                         const NodeType &Node,
                                         const DomainType &InValue) override;

private:
  using Base =
      OldForwardInterProceduralAnalysis<llvm::Function *, llvm::Instruction *,
                                        PointsToGraph>;
  using ResolverReturn = std::optional<std::vector<MethodType>>;

  bool isPointerLike(const llvm::Value *Value) const;
  const llvm::DataLayout *dataLayout() const;
  void seedGlobals(DomainType &State) const;

  PointsToObject classifyObject(const llvm::Value *Value,
                                std::size_t ContextId = 0) const;
  MemoryLocationSet pointsToSetOfValue(const llvm::Value *Value,
                                       const DomainType &State) const;
  MemoryLocationSet evaluateValue(const llvm::Value *Value,
                                  const DomainType &State) const;
  MemoryLocationSet loadFromPointer(const llvm::Value *Pointer,
                                    const DomainType &State) const;
  MemoryLocationSet initializerTargets(const llvm::Constant *Initializer) const;
  void handleStore(const llvm::StoreInst &Store, DomainType &OutValue,
                   const DomainType &InValue) const;
  bool handleMemoryIntrinsicCall(const llvm::CallBase &Call,
                                 DomainType &OutValue,
                                 const DomainType &InValue) const;
  bool handleReallocCall(const llvm::CallBase &Call, DomainType &OutValue,
                         const DomainType &InValue,
                         std::size_t ContextId) const;
  void copyMemoryObject(const MemoryBlock &DstObject,
                        const MemoryBlock &SrcObject, DomainType &OutValue,
                        const DomainType &InValue) const;
  void summarizePointerTargets(const llvm::Value *Pointer, DomainType &OutValue,
                               const DomainType &InValue) const;

  ResolverReturn resolveIndirectTargets(const llvm::CallBase *Call,
                                        const DomainType &State) const;
  std::optional<DomainType> processKnownCall(ContextPtr CallerContext,
                                             NodeType Node,
                                             const MethodType &Method,
                                             const DomainType &EntryValue);
  std::optional<DomainType> processCallTarget(ContextPtr CallerContext,
                                              NodeType Call,
                                              const MethodType &Method,
                                              const DomainType &InValue,
                                              DomainType &Accumulated);
  std::optional<DomainType> flowCall(ContextPtr Context, NodeType Call,
                                     const DomainType &InValue);

  const DefaultLLVMProgramRepresentation *llvmProgram() const;

  const ProgramRepresentation<MethodType, NodeType> &Program;
};

} // namespace llvmir
} // namespace vasco
