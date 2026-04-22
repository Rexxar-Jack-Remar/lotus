#pragma once

#include "Dataflow/VASCO/Support/LLVMPointsToTypes.h"

#include <llvm/IR/DataLayout.h>

#include <optional>
#include <vector>

namespace vasco {
namespace llvmir {

class MemoryModel {
public:
  explicit MemoryModel(const llvm::DataLayout &Layout) : Layout(Layout) {}

  MemoryLayout layoutForStack(const llvm::AllocaInst *Alloca) const;
  MemoryLayout layoutForGlobal(const llvm::GlobalVariable *Global) const;
  MemoryLayout layoutForArgument(const llvm::Argument *Argument) const;
  MemoryLayout layoutForFunction(const llvm::Function *Function) const;
  MemoryLayout layoutForHeapCall(const llvm::CallBase *Call) const;
  MemoryLayout layoutForReallocCall(const llvm::CallBase *Call,
                                    const MemoryBlock &Original) const;
  MemoryLayout layoutForValue(const llvm::Value *Value) const;

  std::optional<MemoryLocation>
  getFieldLocation(const MemoryLocation &Base, const llvm::Value *PointerOperand) const;

  bool isHeapAllocator(const llvm::CallBase *Call) const;
  bool isReallocLikeAllocator(const llvm::CallBase *Call) const;
  bool isMemcpyLikeCall(const llvm::CallBase *Call) const;
  bool isMemmoveLikeCall(const llvm::CallBase *Call) const;
  bool isMemsetLikeCall(const llvm::CallBase *Call) const;

private:
  MemoryLayout layoutForPointeeType(const llvm::Type *Type,
                                    bool CollapseArrays = false) const;
  llvm::Type *inferTypedAllocationFromReturnUses(const llvm::CallBase *Call) const;
  llvm::Type *inferHeapPointeeType(const llvm::CallBase *Call) const;
  llvm::Type *nextBitcastPointeeType(const llvm::Instruction *Instruction) const;
  std::optional<std::uint64_t> constantAllocationSize(const llvm::CallBase *Call) const;
  std::optional<std::int64_t>
  computeOffset(const llvm::Value *PointerOperand, const MemoryLayout &Layout) const;
  bool supportsFieldSensitivity(const llvm::Type *Type) const;
  std::optional<std::uint64_t> allocSizeOf(const llvm::Type *Type) const;

  const llvm::DataLayout &Layout;
};

} // namespace llvmir
} // namespace vasco
