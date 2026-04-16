#pragma once

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Value.h>

namespace concurrency::cuda {

enum class MemorySpace {
  Unknown,
  Host,
  Device,
  Local,
  Shared,
  Global,
  Constant
};

struct MemorySpaceInfo {
  MemorySpace space = MemorySpace::Unknown;
  bool exact = false;
  unsigned address_space = 0;
};

struct BaseObjectInfo {
  llvm::SmallVector<const llvm::Value *, 4> objects;
  bool ambiguous = false;

  const llvm::Value *primary() const {
    return objects.empty() ? nullptr : objects.front();
  }
};

class CUDAMemoryModel {
public:
  static MemorySpaceInfo classify(const llvm::Value *value);
  static const llvm::Value *getCanonicalBase(const llvm::Value *value);
  static BaseObjectInfo getBaseObjectInfo(const llvm::Value *value);
};

} // namespace concurrency::cuda
