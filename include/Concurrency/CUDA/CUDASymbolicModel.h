#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <llvm/IR/Value.h>

namespace concurrency::cuda {

enum class SymbolicValueKind {
  Constant,
  Symbolic,
  DerivedFromBuiltin,
  Unknown
};

enum class BuiltinKind {
  None,
  ThreadIdxX,
  ThreadIdxY,
  ThreadIdxZ,
  BlockIdxX,
  BlockIdxY,
  BlockIdxZ,
  BlockDimX,
  BlockDimY,
  BlockDimZ,
  GridDimX,
  GridDimY,
  GridDimZ,
  LaneId
};

struct SymbolicDimension {
  SymbolicValueKind kind = SymbolicValueKind::Unknown;
  uint64_t constant = 0;
  const llvm::Value *value = nullptr;
};

struct AffineAccessPattern {
  int64_t constant = 0;
  int64_t thread_idx_x = 0;
  int64_t thread_idx_y = 0;
  int64_t thread_idx_z = 0;
  int64_t block_idx_x = 0;
  int64_t block_idx_y = 0;
  int64_t block_idx_z = 0;
  int64_t lane_id = 0;
  bool valid = false;

  // Derive linear thread ID from multidimensional coordinates
  // Assumes row-major: linear_id = z * (blockDim.x * blockDim.y) + y *
  // blockDim.x + x
  static int64_t linearize(int64_t x, int64_t y, int64_t z, int64_t dim_x,
                           int64_t dim_y, int64_t dim_z);

  // Convert linear address to multidimensional
  static void delinearize(int64_t linear, int64_t dim_x, int64_t dim_y,
                          int64_t &x, int64_t &y, int64_t &z);
};

class CUDASymbolicModel {
public:
  static BuiltinKind classifyBuiltin(const llvm::Value *value);
  static bool dependsOnThreadBuiltin(const llvm::Value *value);
  static bool dependsOnBlockBuiltin(const llvm::Value *value);
  static bool dependsOnLaneBuiltin(const llvm::Value *value);
  static std::optional<int64_t> evaluateConstantInt(const llvm::Value *value);
  static AffineAccessPattern
  extractAffineAccessPattern(const llvm::Value *value);
  static SymbolicDimension classifyDimension(const llvm::Value *value);
};

} // namespace concurrency::cuda
