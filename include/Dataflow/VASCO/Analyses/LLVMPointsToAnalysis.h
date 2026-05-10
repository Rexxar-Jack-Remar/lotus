#pragma once

#include "Dataflow/VASCO/Adapters/LLVM/DefaultLLVMProgramRepresentation.h"
#include "Dataflow/VASCO/Core/ProgramRepresentation.h"
#include "Dataflow/VASCO/Solver/OldForwardInterProceduralAnalysis.h"
#include "Dataflow/VASCO/Support/LLVMPointsToMemory.h"

#include <optional>
#include <set>
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

  // ---------------------------------------------------------------------------
  // Convenience query API
  // ---------------------------------------------------------------------------

  /// Compute the meet-over-valid-paths points-to set for `Value`. Aggregates
  /// across every value-context of the enclosing method. Returns the empty set
  /// if `Value` is null or not pointer-typed. May lazily run the analysis (so
  /// callers should call doAnalysis() first for predictable timing).
  MemoryLocationSet getPointsToSet(const llvm::Value *Value) const;

  /// Compute the points-to set of `Value` immediately *before* `AtNode` (i.e.
  /// the in-state of the node), aggregated over all contexts that reach this
  /// node. Useful for flow-sensitive queries at specific program points.
  MemoryLocationSet getPointsToSetBefore(const llvm::Value *Value,
                                         const llvm::Instruction *AtNode) const;

  /// Compute the points-to set of `Value` immediately *after* `AtNode`.
  MemoryLocationSet getPointsToSetAfter(const llvm::Value *Value,
                                        const llvm::Instruction *AtNode) const;

  /// Resolve all callee functions for `Call` over every context that reaches
  /// the call site. The returned set may be empty for unknown / default sites.
  std::set<llvm::Function *> getCallTargets(const llvm::CallBase *Call) const;

  /// Returns true if the call site has been classified as a "default" site
  /// (i.e. the analysis could not resolve any non-summary target).
  bool isDefaultCallSite(const llvm::CallBase *Call) const;

  /// Test whether two pointer-typed values may alias under the
  /// meet-over-valid-paths solution. Returns false if either input is not
  /// pointer-typed or if the points-to sets are disjoint.
  bool mayAlias(const llvm::Value *V1, const llvm::Value *V2) const;

  /// Returns the resolver callback that the analysis exposes for use by other
  /// VASCO analyses. The callback consults the on-the-fly call graph computed
  /// by this analysis and falls back to direct-call resolution when no
  /// information is available. The callback is safe to use only after a
  /// successful `doAnalysis()` call.
  DefaultLLVMProgramRepresentation::ResolveTargetsFn callTargetResolver() const;

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
  void seedGlobalInitializer(const llvm::GlobalVariable *Global,
                             const llvm::Constant *Initializer,
                             const MemoryLocation &Base,
                             DomainType &State) const;
  void handleStore(const llvm::StoreInst &Store, DomainType &OutValue,
                   const DomainType &InValue) const;
  bool handleMemoryIntrinsicCall(const llvm::CallBase &Call,
                                 DomainType &OutValue,
                                 const DomainType &InValue) const;
  bool handleReallocCall(const llvm::CallBase &Call, DomainType &OutValue,
                         const DomainType &InValue,
                         std::size_t ContextId) const;
  /// Applies the summary for an external (declaration-only) call described by
  /// `Summary`. Returns true if the call was handled and no further
  /// interprocedural processing is required.
  bool handleExternalCall(const llvm::CallBase &Call,
                          const ExternalCallSummary &Summary,
                          DomainType &OutValue, const DomainType &InValue,
                          std::size_t ContextId) const;
  void copyMemoryObject(const MemoryBlock &DstObject,
                        const MemoryBlock &SrcObject, DomainType &OutValue,
                        const DomainType &InValue) const;
  /// Weakly "memcpy" points-to information of every object reachable from
  /// `Src` into memory reachable from `Dst`. Unlike `copyMemoryObject`, this
  /// does not require Src/Dst to refer to a specific block - it operates on
  /// the dynamic points-to sets of the pointer arguments.
  void copyThroughPointers(const llvm::Value *Dst, const llvm::Value *Src,
                           DomainType &OutValue,
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
