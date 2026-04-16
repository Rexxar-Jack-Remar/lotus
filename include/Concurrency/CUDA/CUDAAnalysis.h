#pragma once

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/CUDA/CUDAMemoryModel.h"
#include "Concurrency/CUDA/CUDASymbolicModel.h"
#include "Concurrency/Utils/ThreadAPI.h"

#include <array>
#include <cstdint>
#include <memory>
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

enum class SynchronizationScope { None, Warp, Block, Device, System };

enum class RaceKind {
  DataRace,
  AtomicOrderingRisk,
  MissingFence,
  InterKernelHazard
};

enum class LaunchOrderingSource {
  None,
  DeviceSynchronize,
  StreamSynchronize,
  MemoryBarrier,
  ProgramOrder,
  Unknown
};

enum class AliasPrecision { Exact, SymbolicAffine, Ambiguous, NonAffine };

enum class AliasSource { Local, AserPTA, Wrapper };

enum class SynchronizationPrimitive {
  None,
  WarpBarrier,
  BlockBarrier,
  BlockFence,
  DeviceFence,
  SystemFence,
  DeviceSynchronize,
  StreamProgramOrder
};

enum class ParticipationKind { Exact, Conditional, Partial };

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
  size_t sequence = 0;
  SynchronizationPrimitive predecessor = SynchronizationPrimitive::None;
  SynchronizationScope ordering_scope = SynchronizationScope::None;
  bool ordered_after_previous = false;
  LaunchOrderingSource ordering_source = LaunchOrderingSource::None;
  const llvm::Value *stream = nullptr;
  bool stream_known = false;
  bool host_happens_before = false;
};

struct AccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *pointer = nullptr;
  const llvm::Value *base = nullptr;
  llvm::SmallVector<const llvm::Value *, 4> base_objects;
  MemorySpace space = MemorySpace::Unknown;
  bool is_write = false;
  bool is_atomic = false;
  bool is_volatile = false;
  bool has_ambiguous_base = false;
  bool depends_on_thread_idx = false;
  bool depends_on_block_idx = false;
  bool depends_on_lane_id = false;
  bool exact_space = false;
  uint32_t access_size = 0;
  uint32_t address_space = 0;
  UniformityClass uniformity = UniformityClass::Unknown;
  ParticipationScope participation = ParticipationScope::Unknown;
  SynchronizationScope ordering_scope = SynchronizationScope::None;
  bool exact_address = false;
  bool has_fence_relevance = false;
  bool fence_precedes = false;
  AliasPrecision alias_precision = AliasPrecision::NonAffine;
  AliasSource alias_source = AliasSource::Local;
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

enum class TransferKind {
  Unknown,
  HostToDevice,
  DeviceToHost,
  DeviceToDevice,
  HostToHost
};

struct MemoryTransferInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *src = nullptr;
  const llvm::Value *dst = nullptr;
  MemorySpace src_space = MemorySpace::Unknown;
  MemorySpace dst_space = MemorySpace::Unknown;
  TransferKind kind = TransferKind::Unknown;
  bool is_async = false;
  uint64_t size = 0;
};

struct ConstantAccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *base = nullptr;
  uint32_t access_size = 0;
  bool strided = false;
  int64_t stride = 0;
};

struct UnifiedMemoryInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *ptr = nullptr;
  uint64_t size = 0;
  bool is_managed = false;
  bool is_prefetch = false;
  int device_id = -1;
};

struct TextureAccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *texref = nullptr;
  uint32_t dimensions = 0;
  bool is_write = false;
};

struct SurfaceAccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *surfref = nullptr;
  uint32_t dimensions = 0;
  bool is_write = false;
};

struct RaceInfo {
  const llvm::Instruction *first = nullptr;
  const llvm::Instruction *second = nullptr;
  const llvm::Value *base = nullptr;
  llvm::SmallVector<const llvm::Value *, 4> bases;
  MemorySpace space = MemorySpace::Unknown;
  bool same_block_only = false;
  bool cross_block = false;
  bool symbolic = false;
  RaceKind kind = RaceKind::DataRace;
  SynchronizationScope scope = SynchronizationScope::None;
  const char *ordering_reason = nullptr;
  bool exact = false;
  const llvm::Instruction *ordering_inst = nullptr;
  SynchronizationScope required_fence_scope = SynchronizationScope::None;
  AliasPrecision alias_precision = AliasPrecision::NonAffine;
  AliasSource alias_source = AliasSource::Local;
  SynchronizationPrimitive missing_ordering = SynchronizationPrimitive::None;
  double confidence = 0.0;
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

struct SynchronizationRecord {
  const llvm::Instruction *inst = nullptr;
  SynchronizationPrimitive primitive = SynchronizationPrimitive::None;
  SynchronizationScope scope = SynchronizationScope::None;
  bool orders_memory = false;
  bool execution_rendezvous = false;
  ParticipationScope participating_threads = ParticipationScope::Unknown;
  ParticipationKind participation = ParticipationKind::Partial;
  llvm::SmallVector<const llvm::BasicBlock *, 8> preceding_blocks;
  llvm::SmallVector<const llvm::BasicBlock *, 8> following_blocks;
  bool exact = false;
};

struct InterKernelRaceInfo {
  const llvm::Instruction *first_launch = nullptr;
  const llvm::Instruction *second_launch = nullptr;
  const llvm::Function *first_kernel = nullptr;
  const llvm::Function *second_kernel = nullptr;
  const llvm::Value *shared_base = nullptr;
  bool ordered = false;
  RaceKind kind = RaceKind::InterKernelHazard;
  const char *ordering_reason = nullptr;
  bool symbolic = false;
  const llvm::Instruction *ordering_inst = nullptr;
  LaunchOrderingSource ordering_source = LaunchOrderingSource::None;
  const llvm::Value *stream = nullptr;
  bool stream_known = false;
  AliasPrecision alias_precision = AliasPrecision::NonAffine;
  AliasSource alias_source = AliasSource::Local;
  SynchronizationPrimitive missing_ordering = SynchronizationPrimitive::None;
  SynchronizationScope required_fence_scope = SynchronizationScope::None;
  double confidence = 0.0;
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
  size_t transfer_count = 0;
  bool has_uncoalesced_constant = false;
  bool has_texture_access = false;
  bool has_surface_access = false;
  llvm::SmallVector<DivergenceRegion, 4> divergence_regions;
  llvm::SmallVector<BankConflictInfo, 4> bank_conflicts;
  llvm::SmallVector<CoalescingInfo, 4> coalescing_issues;
  llvm::SmallVector<RaceInfo, 4> shared_races;
  llvm::SmallVector<RaceInfo, 4> global_races;
  llvm::SmallVector<BarrierMismatchInfo, 4> barrier_mismatches;
  llvm::SmallVector<VolatileMissingInfo, 4> volatile_missing;
  llvm::SmallVector<WarpUniformInfo, 4> warp_uniform_regions;
  llvm::SmallVector<SynchronizationRecord, 4> synchronizations;
  llvm::SmallVector<MemoryTransferInfo, 4> memory_transfers;
  llvm::SmallVector<ConstantAccessInfo, 4> constant_accesses;
  llvm::SmallVector<TextureAccessInfo, 4> texture_accesses;
  llvm::SmallVector<SurfaceAccessInfo, 4> surface_accesses;
  std::vector<AccessInfo> accesses;
};

class CUDAAnalysis {
public:
  explicit CUDAAnalysis(llvm::Module &module,
                        lotus::AliasAnalysisWrapper *alias_analysis,
                        DeviceConfig config = DeviceConfig{});
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
  const std::vector<MemoryTransferInfo> &getMemoryTransfers() const {
    return m_memory_transfers;
  }
  const DeviceConfig &getDeviceConfig() const { return m_device_config; }

  static MemorySpace classifyMemorySpace(const llvm::Value *value);
  static const char *toString(MemorySpace space);
  static const char *toString(CoalescingQuality quality);
  static const char *toString(UniformityClass uniformity);
  static const char *toString(SynchronizationScope scope);
  static const char *toString(RaceKind kind);
  static const char *toString(LaunchOrderingSource source);
  static const char *toString(AliasPrecision precision);
  static const char *toString(AliasSource source);
  static const char *toString(SynchronizationPrimitive primitive);

private:
  llvm::Module &m_module;
  ThreadAPI *m_thread_api;
  lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;
  std::unique_ptr<lotus::AliasAnalysisWrapper> m_owned_alias_analysis;
  DeviceConfig m_device_config;
  std::vector<KernelLaunchInfo> m_launches;
  std::vector<KernelSummary> m_kernel_summaries;
  std::vector<InterKernelRaceInfo> m_inter_kernel_races;
  std::vector<MemoryTransferInfo> m_memory_transfers;
  std::vector<UnifiedMemoryInfo> m_unified_memory;
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
  void analyzeSynchronization(KernelSummary &summary,
                              const llvm::Function *kernel);
  void analyzeInterKernelRaces();
  void analyzeMemoryTransfers();
  void analyzeConstantAccesses(KernelSummary &summary);

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
  static UniformityClass classifyUniformity(const llvm::Value *value) {
    return CUDASymbolicModel::classifyUniformity(value);
  }
  static ParticipationScope classifyParticipation(const llvm::Value *value) {
    return CUDASymbolicModel::classifyParticipation(value);
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
  void initializeDefaultAliasAnalysis();
  bool hasAliasAnalysis() const;
  static LaunchDimensions getLaunchDimensions(const llvm::Instruction *launch);
};

} // namespace concurrency::cuda
