#pragma once

#include "Concurrency/CUDA/CUDAMemoryModel.h"
#include "Concurrency/CUDA/CUDASymbolicModel.h"
#include "Concurrency/Utils/ThreadAPI.h"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace concurrency::cuda {

enum class CoalescingQuality {
  Unknown,
  FullyCoalesced,
  PartiallyCoalesced,
  Uncoalesced
};

struct LaunchDimensions {
  std::array<SymbolicDimension, 3> grid{};
  std::array<SymbolicDimension, 3> block{};

  bool hasSymbolicGrid() const;
  bool hasSymbolicBlock() const;
};

struct DeviceConfig {
  uint32_t warp_size = 32;
  uint32_t shared_bank_count = 32;
  uint32_t shared_bank_width = 4;
  uint32_t global_transaction_bytes = 128;
};

struct KernelLaunchInfo {
  const llvm::Instruction *launch = nullptr;
  const llvm::Function *kernel = nullptr;
  LaunchDimensions dimensions;
};

struct AccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *pointer = nullptr;
  const llvm::Value *base = nullptr;
  MemorySpace space = MemorySpace::Unknown;
  bool is_write = false;
  bool is_atomic = false;
  bool is_volatile = false;
  bool depends_on_thread_idx = false;
  bool depends_on_block_idx = false;
  bool depends_on_lane_id = false;
  bool exact_space = false;
  uint32_t access_size = 0;
  uint32_t address_space = 0;
  AffineAccessPattern address_pattern;
};

struct DivergenceRegion {
  const llvm::Instruction *branch = nullptr;
  const llvm::BasicBlock *merge_block = nullptr;
  llvm::SmallVector<const llvm::Instruction *, 4> nested_barriers;
  llvm::SmallVector<const llvm::BasicBlock *, 8> region_blocks;
  bool depends_on_thread_idx = false;
  bool depends_on_block_idx = false;
  bool depends_on_lane_id = false;
};

struct BankConflictInfo {
  const llvm::Instruction *inst = nullptr;
  uint32_t bank_count = 0;
  uint32_t bank_width = 0;
  uint32_t conflict_degree = 0;
  uint32_t threads_per_bank = 0;
  uint32_t bank_stride_bytes = 0;
  uint32_t unique_banks = 0;
  bool is_broadcast = false;
  bool exact = false;
};

struct CoalescingInfo {
  const llvm::Instruction *inst = nullptr;
  CoalescingQuality quality = CoalescingQuality::Unknown;
  uint32_t estimated_transactions = 0;
  uint32_t transaction_bytes = 0;
  uint32_t covered_bytes = 0;
  uint32_t participating_lanes = 0;
  uint32_t unique_segments = 0;
};

struct RaceInfo {
  const llvm::Instruction *first = nullptr;
  const llvm::Instruction *second = nullptr;
  const llvm::Value *base = nullptr;
  MemorySpace space = MemorySpace::Unknown;
  bool same_block_only = false;
  bool cross_block = false;
  bool symbolic = false;
};

struct BarrierMismatchInfo {
  const llvm::Instruction *branch = nullptr;
  const llvm::Instruction *barrier = nullptr;
};

struct WarpUniformInfo {
  const llvm::Instruction *branch = nullptr;
  bool uniform_within_warp = false;
  bool uniform_within_block = false;
  llvm::SmallVector<const llvm::BasicBlock *, 8> uniform_blocks;
};

struct BarrierPhaseInfo {
  const llvm::Instruction *barrier = nullptr;
  llvm::SmallVector<const llvm::BasicBlock *, 8> preceding_blocks;
  llvm::SmallVector<const llvm::BasicBlock *, 8> following_blocks;
  bool all_threads_reach = false;
};

struct InterKernelRaceInfo {
  const llvm::Instruction *first_launch = nullptr;
  const llvm::Instruction *second_launch = nullptr;
  const llvm::Function *first_kernel = nullptr;
  const llvm::Function *second_kernel = nullptr;
  const llvm::Value *shared_base = nullptr;
  bool ordered = false;
};

struct VolatileMissingInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *base = nullptr;
};

struct KernelSummary {
  const llvm::Function *kernel = nullptr;
  LaunchDimensions dimensions;
  bool has_warp_divergence = false;
  bool has_bank_conflict = false;
  bool has_uncoalesced_access = false;
  bool has_barrier_mismatch = false;
  bool has_shared_race = false;
  bool has_global_race = false;
  bool has_volatile_missing = false;
  size_t shared_access_count = 0;
  size_t device_access_count = 0;
  size_t global_access_count = 0;
  size_t constant_access_count = 0;
  size_t local_access_count = 0;
  size_t atomic_count = 0;
  llvm::SmallVector<DivergenceRegion, 4> divergence_regions;
  llvm::SmallVector<BankConflictInfo, 4> bank_conflicts;
  llvm::SmallVector<CoalescingInfo, 4> coalescing_issues;
  llvm::SmallVector<RaceInfo, 4> shared_races;
  llvm::SmallVector<RaceInfo, 4> global_races;
  llvm::SmallVector<BarrierMismatchInfo, 4> barrier_mismatches;
  llvm::SmallVector<VolatileMissingInfo, 4> volatile_missing;
  llvm::SmallVector<WarpUniformInfo, 4> warp_uniform_regions;
  llvm::SmallVector<BarrierPhaseInfo, 4> barrier_phases;
  std::vector<AccessInfo> accesses;
};

class CUDAAnalysis {
public:
  explicit CUDAAnalysis(llvm::Module &module,
                        DeviceConfig config = DeviceConfig{});

  void runAnalysis();

  const std::vector<KernelLaunchInfo> &getLaunches() const {
    return m_launches;
  }
  const std::vector<KernelSummary> &getKernelSummaries() const {
    return m_kernel_summaries;
  }
  const std::vector<InterKernelRaceInfo> &getInterKernelRaces() const {
    return m_inter_kernel_races;
  }
  const DeviceConfig &getDeviceConfig() const { return m_device_config; }

  static MemorySpace classifyMemorySpace(const llvm::Value *value);
  static const char *toString(MemorySpace space);
  static const char *toString(CoalescingQuality quality);

private:
  llvm::Module &m_module;
  ThreadAPI *m_thread_api;
  DeviceConfig m_device_config;
  std::vector<KernelLaunchInfo> m_launches;
  std::vector<KernelSummary> m_kernel_summaries;
  std::vector<InterKernelRaceInfo> m_inter_kernel_races;
  std::unordered_map<const llvm::Function *, size_t> m_kernel_index;

  void analyzeKernel(const llvm::Function *kernel,
                     const KernelLaunchInfo *launch);
  void recordAccess(KernelSummary &summary, const llvm::Instruction *inst,
                    const llvm::Value *pointer, bool is_write);
  void analyzeDivergence(KernelSummary &summary, const llvm::Function *kernel);
  void analyzeRaces(KernelSummary &summary);
  void analyzeVolatile(KernelSummary &summary);
  void analyzeWarpUniformity(KernelSummary &summary,
                             const llvm::Function *kernel);
  void analyzeBarrierPhases(KernelSummary &summary,
                            const llvm::Function *kernel);
  void analyzeInterKernelRaces();

  static const llvm::Value *getMemoryOperand(const llvm::Instruction *inst);
  static const llvm::Value *getCanonicalBase(const llvm::Value *value) {
    return CUDAMemoryModel::getCanonicalBase(value);
  }
  static BuiltinKind classifyBuiltin(const llvm::Value *value) {
    return CUDASymbolicModel::classifyBuiltin(value);
  }
  static bool dependsOnThreadBuiltin(const llvm::Value *value) {
    return CUDASymbolicModel::dependsOnThreadBuiltin(value);
  }
  static bool dependsOnBlockBuiltin(const llvm::Value *value) {
    return CUDASymbolicModel::dependsOnBlockBuiltin(value);
  }
  static bool dependsOnLaneBuiltin(const llvm::Value *value) {
    return CUDASymbolicModel::dependsOnLaneBuiltin(value);
  }
  static std::optional<int64_t> evaluateConstantInt(const llvm::Value *value) {
    return CUDASymbolicModel::evaluateConstantInt(value);
  }
  static AffineAccessPattern
  extractAffineAccessPattern(const llvm::Value *value) {
    return CUDASymbolicModel::extractAffineAccessPattern(value);
  }
  static SymbolicDimension classifyDimension(const llvm::Value *value) {
    return CUDASymbolicModel::classifyDimension(value);
  }
  static LaunchDimensions getLaunchDimensions(const llvm::Instruction *launch);
};

} // namespace concurrency::cuda
