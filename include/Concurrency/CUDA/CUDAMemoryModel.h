#pragma once

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

class CUDAMemoryModel {
public:
  static MemorySpaceInfo classify(const llvm::Value *value);
  static const llvm::Value *getCanonicalBase(const llvm::Value *value);
};

} // namespace concurrency::cuda
