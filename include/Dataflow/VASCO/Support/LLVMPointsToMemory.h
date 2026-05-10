#pragma once

#include "Dataflow/VASCO/Support/LLVMPointsToTypes.h"

#include <llvm/IR/DataLayout.h>

#include <optional>
#include <vector>

namespace vasco {
namespace llvmir {

/// Classification of an external (declaration-only) call's effect on the
/// points-to state. Inspired by TPA's `ExternalCallAnalysis` pointer effect
/// table, but kept self-contained so VASCO does not depend on Lotus' annotation
/// loader.
enum class ExternalCallEffect {
  /// Call is irrelevant to pointer state (no pointer args, no pointer return,
  /// non-mutating, e.g. `printf`, `puts`, `fprintf` with constant format,
  /// `getpid`).
  Noop,

  /// Call allocates a fresh, possibly typed heap object (`malloc`, `calloc`,
  /// `_Znwm`, ...). Already handled by `isHeapAllocator`.
  Alloc,

  /// Call returns a fresh duplicate of one of its pointer arguments (e.g.
  /// `strdup`, `strndup`). The destination is a new heap allocation whose
  /// contents are a (weakly) summarized copy of the source.
  StringDup,

  /// Call copies bytes from one buffer to another (memcpy/memmove and string
  /// copy variants like `strcpy`, `strncpy`, `strcat`, `strncat`).
  Memcopy,

  /// Call writes a fixed pattern into the destination buffer (memset, bzero).
  Memset,

  /// Call returns a pointer aliasing one of its arguments (e.g. `strchr`,
  /// `strrchr`, `strstr`, `memchr`, `strpbrk`). Return value points to the
  /// same objects as the source argument.
  ReturnsArgument,

  /// Call returns an opaque pointer (e.g. `fopen`, `getenv`, `dlopen`). The
  /// return value is summarized; arguments are not modified through the call.
  OpaqueReturn,

  /// Process termination (`exit`, `abort`, `_Exit`, ...). No successors need
  /// updates beyond standard CFG flow.
  Exit,

  /// Function is unknown - the analysis must conservatively summarize all
  /// pointer-typed arguments (and any pointer returned).
  Unknown,
};

/// Per-call modelling parameters returned alongside the effect.
struct ExternalCallSummary {
  ExternalCallEffect Effect = ExternalCallEffect::Unknown;
  unsigned DestArgIndex = 0;
  unsigned SrcArgIndex = 0;
  bool ReturnsDestArg = false;
};

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

  std::optional<MemoryLocation>
  getFieldLocationFromOffset(const MemoryLocation &Base,
                             std::int64_t AdditionalOffset) const;

  bool isHeapAllocator(const llvm::CallBase *Call) const;
  bool isReallocLikeAllocator(const llvm::CallBase *Call) const;
  bool isMemcpyLikeCall(const llvm::CallBase *Call) const;
  bool isMemmoveLikeCall(const llvm::CallBase *Call) const;
  bool isMemsetLikeCall(const llvm::CallBase *Call) const;

  /// Classify a call to a declaration-only function. Returns a summary that
  /// describes how points-to state should be updated. Direct heap/memcpy/etc.
  /// calls are classified accordingly so callers can dispatch uniformly.
  ExternalCallSummary classifyExternalCall(const llvm::CallBase *Call) const;

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
