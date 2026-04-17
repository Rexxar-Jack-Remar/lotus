#include "Concurrency/CUDA/CUDAAnalysis.h"

#include "CUDAAnalysisInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace concurrency::cuda {

namespace {

struct RaceDecision {
  bool aliases = false;
  bool symbolic = false;
  AliasPrecision precision = AliasPrecision::NonAffine;
  AliasSource source = AliasSource::Local;
  double confidence = 0.0;
};

static bool isSharedRaceRelevant(const AccessInfo &access) {
  return access.space == MemorySpace::Shared && access.base &&
         !access.is_atomic;
}

static bool isGlobalRaceRelevant(const AccessInfo &access) {
  return (access.space == MemorySpace::Global ||
          access.space == MemorySpace::Device) &&
         access.base;
}

static AliasPrecision getAliasPrecision(const AccessInfo &access) {
  if (access.has_ambiguous_base) {
    return AliasPrecision::Ambiguous;
  }
  if (access.address_pattern.valid) {
    return access.address_pattern.exact ? AliasPrecision::Exact
                                        : AliasPrecision::SymbolicAffine;
  }
  return AliasPrecision::NonAffine;
}

static bool sharesBaseObject(const AccessInfo &lhs, const AccessInfo &rhs) {
  if (lhs.base_objects.empty() && rhs.base_objects.empty()) {
    return lhs.base && rhs.base && lhs.base == rhs.base;
  }
  if (lhs.base_objects.empty()) {
    return rhs.base && lhs.base && lhs.base == rhs.base;
  }
  if (rhs.base_objects.empty()) {
    return lhs.base && rhs.base && lhs.base == rhs.base;
  }
  for (const Value *lhs_base : lhs.base_objects) {
    for (const Value *rhs_base : rhs.base_objects) {
      if (lhs_base == rhs_base) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] static bool sharesStream(const KernelLaunchInfo &lhs,
                                          const KernelLaunchInfo &rhs) {
  if (!lhs.stream_known || !rhs.stream_known) {
    return false;
  }
  return lhs.stream == rhs.stream;
}

[[maybe_unused]] static bool accessesConflict(const AccessInfo &lhs,
                                              const AccessInfo &rhs) {
  return sharesBaseObject(lhs, rhs) && (lhs.is_write || rhs.is_write);
}

static SmallVector<const Value *, 4> mergeBaseObjects(const AccessInfo &lhs,
                                                      const AccessInfo &rhs) {
  SmallVector<const Value *, 4> bases;
  SmallPtrSet<const Value *, 8> seen;
  for (const Value *base : lhs.base_objects) {
    if (base && seen.insert(base).second) {
      bases.push_back(base);
    }
  }
  for (const Value *base : rhs.base_objects) {
    if (base && seen.insert(base).second) {
      bases.push_back(base);
    }
  }
  if (bases.empty()) {
    if (lhs.base) {
      bases.push_back(lhs.base);
    } else if (rhs.base) {
      bases.push_back(rhs.base);
    }
  }
  return bases;
}

static bool mustReachBarrier(const Instruction *barrier,
                             const Function *kernel) {
  const BasicBlock *barrier_block = barrier ? barrier->getParent() : nullptr;
  const BasicBlock *entry =
      kernel && !kernel->empty() ? &kernel->getEntryBlock() : nullptr;
  if (!barrier_block || !entry) {
    return false;
  }

  SmallVector<const BasicBlock *, 8> worklist;
  SmallPtrSet<const BasicBlock *, 16> visited;
  worklist.push_back(entry);
  while (!worklist.empty()) {
    const BasicBlock *current = worklist.pop_back_val();
    if (!visited.insert(current).second) {
      continue;
    }
    if (current == barrier_block) {
      continue;
    }
    if (succ_empty(current)) {
      return false;
    }
    for (const BasicBlock *succ : successors(current)) {
      worklist.push_back(succ);
    }
  }
  return true;
}

static const Instruction *getFirstBarrierInBlock(const BasicBlock *bb,
                                                 ThreadAPI *thread_api) {
  if (!bb) {
    return nullptr;
  }
  for (const Instruction &inst : *bb) {
    const auto *call = dyn_cast<CallBase>(&inst);
    if (!call) {
      continue;
    }
    ThreadAPI::TD_TYPE type = thread_api->getType(call);
    if (type == ThreadAPI::TD_CUDA_BARRIER ||
        type == ThreadAPI::TD_CUDA_WARP_BARRIER) {
      return &inst;
    }
  }
  return nullptr;
}

[[maybe_unused]] static bool isFenceInstruction(const Instruction *inst,
                                                ThreadAPI *thread_api) {
  const auto *call = dyn_cast<CallBase>(inst);
  if (!call) {
    return false;
  }
  return thread_api->getType(call) == ThreadAPI::TD_CUDA_MEMORY_BARRIER;
}

[[maybe_unused]] static bool
isExecutionSynchronizingPrimitive(SynchronizationPrimitive primitive) {
  switch (primitive) {
  case SynchronizationPrimitive::WarpBarrier:
  case SynchronizationPrimitive::BlockBarrier:
  case SynchronizationPrimitive::DeviceSynchronize:
  case SynchronizationPrimitive::StreamProgramOrder:
    return true;
  default:
    return false;
  }
}

static bool ordersMemorySpace(SynchronizationPrimitive primitive,
                              MemorySpace space) {
  switch (primitive) {
  case SynchronizationPrimitive::WarpBarrier:
  case SynchronizationPrimitive::BlockBarrier:
    return space == MemorySpace::Shared || space == MemorySpace::Global ||
           space == MemorySpace::Device;
  case SynchronizationPrimitive::BlockFence:
  case SynchronizationPrimitive::DeviceFence:
  case SynchronizationPrimitive::DeviceSynchronize:
  case SynchronizationPrimitive::StreamProgramOrder:
    return space == MemorySpace::Global || space == MemorySpace::Device;
  case SynchronizationPrimitive::SystemFence:
    return space == MemorySpace::Global || space == MemorySpace::Device ||
           space == MemorySpace::Host;
  case SynchronizationPrimitive::None:
    return false;
  }
  return false;
}

static SmallVector<const BasicBlock *, 8>
collectReachableBlocks(const BasicBlock *start, const BasicBlock *stop) {
  SmallVector<const BasicBlock *, 8> blocks;
  if (!start) {
    return blocks;
  }

  SmallVector<const BasicBlock *, 8> worklist;
  SmallPtrSet<const BasicBlock *, 16> visited;
  worklist.push_back(start);

  while (!worklist.empty()) {
    const BasicBlock *current = worklist.pop_back_val();
    if (!visited.insert(current).second) {
      continue;
    }
    if (current == stop) {
      continue;
    }
    blocks.push_back(current);
    for (const BasicBlock *succ : successors(current)) {
      worklist.push_back(succ);
    }
  }

  return blocks;
}

static uint32_t getConcreteExtent(const SymbolicDimension &dim,
                                  uint32_t fallback) {
  if (dim.kind == SymbolicValueKind::Constant && dim.constant != 0) {
    return static_cast<uint32_t>(dim.constant);
  }
  return fallback;
}

static uint32_t getThreadsPerBlock(const LaunchDimensions &dims) {
  uint64_t threads =
      static_cast<uint64_t>(getConcreteExtent(dims.block[0], 1)) *
      static_cast<uint64_t>(getConcreteExtent(dims.block[1], 1)) *
      static_cast<uint64_t>(getConcreteExtent(dims.block[2], 1));
  if (threads == 0) {
    return 1;
  }
  return static_cast<uint32_t>(
      std::min<uint64_t>(threads, std::numeric_limits<uint32_t>::max()));
}

static uint32_t getActiveLaneCount(const LaunchDimensions &dims,
                                   const DeviceConfig &config) {
  if (dims.hasSymbolicBlock()) {
    return config.warp_size;
  }
  return std::min(config.warp_size, getThreadsPerBlock(dims));
}

[[maybe_unused]] static bool isBlockUniformBuiltin(BuiltinKind kind) {
  switch (kind) {
  case BuiltinKind::BlockIdxX:
  case BuiltinKind::BlockIdxY:
  case BuiltinKind::BlockIdxZ:
  case BuiltinKind::BlockDimX:
  case BuiltinKind::BlockDimY:
  case BuiltinKind::BlockDimZ:
  case BuiltinKind::GridDimX:
  case BuiltinKind::GridDimY:
  case BuiltinKind::GridDimZ:
    return true;
  default:
    return false;
  }
}

static bool isWarpUniformValue(const Value *value) {
  return CUDASymbolicModel::classifyUniformity(value) <=
         UniformityClass::WarpUniform;
}

static bool isReachable(const BasicBlock *from, const BasicBlock *to) {
  if (!from || !to) {
    return false;
  }
  if (from == to) {
    return true;
  }

  SmallVector<const BasicBlock *, 8> worklist;
  SmallPtrSet<const BasicBlock *, 16> visited;
  worklist.push_back(from);
  while (!worklist.empty()) {
    const BasicBlock *current = worklist.pop_back_val();
    if (!visited.insert(current).second) {
      continue;
    }
    for (const BasicBlock *succ : successors(current)) {
      if (succ == to) {
        return true;
      }
      worklist.push_back(succ);
    }
  }
  return false;
}

static bool comesBeforeInBlock(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs->getParent() != rhs->getParent()) {
    return false;
  }
  for (const Instruction &inst : *lhs->getParent()) {
    if (&inst == lhs) {
      return true;
    }
    if (&inst == rhs) {
      return false;
    }
  }
  return false;
}

struct LaneCoordinates {
  int64_t tid_x = 0;
  int64_t tid_y = 0;
  int64_t tid_z = 0;
};

static LaneCoordinates getLaneCoordinates(uint32_t lane,
                                          const LaunchDimensions &dims) {
  LaneCoordinates coords;
  int64_t dim_x = getConcreteExtent(dims.block[0], 32);
  int64_t dim_y = getConcreteExtent(dims.block[1], 1);
  int64_t dim_z = getConcreteExtent(dims.block[2], 1);
  AffineAccessPattern::delinearize(lane, dim_x, dim_y, coords.tid_x,
                                   coords.tid_y, coords.tid_z);
  if (coords.tid_z >= dim_z) {
    coords.tid_z = std::max<int64_t>(0, dim_z - 1);
  }
  return coords;
}

static std::optional<int64_t>
evaluateAddressForThread(const AffineAccessPattern &pattern,
                         const LaunchDimensions &dims, int64_t tid_x,
                         int64_t tid_y, int64_t tid_z, int64_t block_x,
                         int64_t block_y, int64_t block_z) {
  if (!pattern.valid) {
    return std::nullopt;
  }
  int64_t dim_x = getConcreteExtent(dims.block[0], 32);
  int64_t dim_y = getConcreteExtent(dims.block[1], 1);
  int64_t dim_z = getConcreteExtent(dims.block[2], 1);
  int64_t linear_tid =
      AffineAccessPattern::linearize(tid_x, tid_y, tid_z, dim_x, dim_y, dim_z);
  return pattern.constant + pattern.thread_idx_x * tid_x +
         pattern.thread_idx_y * tid_y + pattern.thread_idx_z * tid_z +
         pattern.block_idx_x * block_x + pattern.block_idx_y * block_y +
         pattern.block_idx_z * block_z + pattern.lane_id * (linear_tid % 32);
}

[[maybe_unused]] static std::optional<int64_t>
evaluateAddressForThread(const AffineAccessPattern &pattern, int64_t tid_x,
                         int64_t block_x) {
  LaunchDimensions dummy_dims;
  dummy_dims.block[0].kind = SymbolicValueKind::Constant;
  dummy_dims.block[0].constant = 32;
  return evaluateAddressForThread(pattern, dummy_dims, tid_x, 0, 0, block_x, 0,
                                  0);
}

static bool hasDistinctThreadAlias(const AccessInfo &lhs, const AccessInfo &rhs,
                                   const LaunchDimensions &dims,
                                   const DeviceConfig &config,
                                   bool allow_cross_block) {
  if (lhs.address_pattern.valid && rhs.address_pattern.valid) {
    const AffineAccessPattern &lhs_pattern = lhs.address_pattern;
    const AffineAccessPattern &rhs_pattern = rhs.address_pattern;
    const bool lhs_thread_private =
        lhs_pattern.thread_idx_x == 1 && lhs_pattern.thread_idx_y == 0 &&
        lhs_pattern.thread_idx_z == 0 && lhs_pattern.block_idx_x == 0 &&
        lhs_pattern.block_idx_y == 0 && lhs_pattern.block_idx_z == 0 &&
        lhs_pattern.lane_id == 0;
    const bool rhs_thread_private =
        rhs_pattern.thread_idx_x == 1 && rhs_pattern.thread_idx_y == 0 &&
        rhs_pattern.thread_idx_z == 0 && rhs_pattern.block_idx_x == 0 &&
        rhs_pattern.block_idx_y == 0 && rhs_pattern.block_idx_z == 0 &&
        rhs_pattern.lane_id == 0;
    if (lhs_thread_private && rhs_thread_private) {
      return false;
    }
  }
  if (!lhs.address_pattern.valid || !rhs.address_pattern.valid) {
    return lhs.depends_on_thread_idx || rhs.depends_on_thread_idx ||
           lhs.depends_on_block_idx || rhs.depends_on_block_idx;
  }

  const uint32_t lanes = getActiveLaneCount(dims, config);
  const uint32_t block_count =
      allow_cross_block
          ? std::min<uint32_t>(2, getConcreteExtent(dims.grid[0], 2))
          : 1;

  for (uint32_t block_l = 0; block_l < block_count; ++block_l) {
    for (uint32_t block_r = 0; block_r < block_count; ++block_r) {
      if (!allow_cross_block && block_l != block_r) {
        continue;
      }
      for (uint32_t tid_l = 0; tid_l < lanes; ++tid_l) {
        for (uint32_t tid_r = 0; tid_r < lanes; ++tid_r) {
          if (tid_l == tid_r && block_l == block_r) {
            continue;
          }
          const LaneCoordinates lane_l = getLaneCoordinates(tid_l, dims);
          const LaneCoordinates lane_r = getLaneCoordinates(tid_r, dims);
          const auto lhs_addr = evaluateAddressForThread(
              lhs.address_pattern, dims, lane_l.tid_x, lane_l.tid_y,
              lane_l.tid_z, block_l, 0, 0);
          const auto rhs_addr = evaluateAddressForThread(
              rhs.address_pattern, dims, lane_r.tid_x, lane_r.tid_y,
              lane_r.tid_z, block_r, 0, 0);
          if (lhs_addr && rhs_addr && lhs_addr == rhs_addr) {
            return true;
          }
        }
      }
    }
  }

  return false;
}

static RaceDecision
evaluateRaceDecision(const AccessInfo &lhs, const AccessInfo &rhs,
                     const LaunchDimensions &dims, const DeviceConfig &config,
                     bool allow_cross_block,
                     const detail::AliasQueryResult &alias) {
  RaceDecision decision;
  decision.aliases =
      alias.relation != AliasResult::NoAlias &&
      hasDistinctThreadAlias(lhs, rhs, dims, config, allow_cross_block);
  decision.symbolic = dims.hasSymbolicGrid() || dims.hasSymbolicBlock() ||
                      !lhs.exact_address || !rhs.exact_address ||
                      lhs.has_ambiguous_base || rhs.has_ambiguous_base;
  decision.precision =
      std::max(alias.precision,
               std::max(getAliasPrecision(lhs), getAliasPrecision(rhs)));
  decision.source = alias.source;
  decision.confidence = decision.aliases ? 0.9 : 0.0;
  if (alias.relation == AliasResult::MustAlias) {
    decision.confidence = 0.98;
  } else if (alias.relation == AliasResult::PartialAlias) {
    decision.confidence *= 0.85;
  }
  if (decision.precision == AliasPrecision::SymbolicAffine) {
    decision.confidence *= 0.75;
  } else if (decision.precision == AliasPrecision::Ambiguous) {
    decision.confidence *= 0.55;
  } else if (decision.precision == AliasPrecision::NonAffine) {
    decision.confidence *= 0.4;
  }
  if (decision.symbolic) {
    decision.confidence *= 0.8;
  }
  return decision;
}

static std::optional<BankConflictInfo>
computeBankConflict(const AccessInfo &access, const LaunchDimensions &dims,
                    const DeviceConfig &config) {
  if (!access.address_pattern.valid || access.access_size == 0) {
    return std::nullopt;
  }

  const uint32_t lanes = getActiveLaneCount(dims, config);
  const uint32_t bank_width = config.shared_bank_width;
  const uint32_t bank_count = config.shared_bank_count;
  SmallVector<uint32_t, 32> occupancies(bank_count, 0);
  SmallVector<uint32_t, 32> lane_banks;
  lane_banks.reserve(lanes);
  bool all_same_address = true;
  std::optional<int64_t> first_addr;

  for (uint32_t lane = 0; lane < lanes; ++lane) {
    const LaneCoordinates coords = getLaneCoordinates(lane, dims);
    auto addr =
        evaluateAddressForThread(access.address_pattern, dims, coords.tid_x,
                                 coords.tid_y, coords.tid_z, 0, 0, 0);
    if (!addr) {
      return std::nullopt;
    }
    if (!first_addr) {
      first_addr = *addr;
    } else if (*first_addr != *addr) {
      all_same_address = false;
    }

    SmallVector<bool, 32> touched_banks(bank_count, false);
    for (uint32_t byte = 0; byte < access.access_size; ++byte) {
      const int64_t absolute = *addr + byte;
      const uint32_t bank =
          static_cast<uint32_t>((absolute / bank_width) % bank_count);
      touched_banks[bank] = true;
      if (byte == 0) {
        lane_banks.push_back(bank);
      }
    }
    for (uint32_t bank = 0; bank < bank_count; ++bank) {
      if (touched_banks[bank]) {
        ++occupancies[bank];
      }
    }
  }

  uint32_t max_occupancy = 0;
  for (uint32_t occupancy : occupancies) {
    max_occupancy = std::max(max_occupancy, occupancy);
  }
  SmallVector<bool, 32> seen_bank(bank_count, false);
  uint32_t unique_bank_count = 0;
  for (uint32_t bank : lane_banks) {
    if (!seen_bank[bank]) {
      seen_bank[bank] = true;
      ++unique_bank_count;
    }
  }
  if (all_same_address || max_occupancy <= 1) {
    return std::nullopt;
  }

  BankConflictInfo info;
  info.inst = access.inst;
  info.bank_count = bank_count;
  info.bank_width = bank_width;
  info.conflict_degree = max_occupancy;
  info.threads_per_bank = max_occupancy;
  info.bank_stride_bytes =
      static_cast<uint32_t>(std::abs(access.address_pattern.thread_idx_x));
  info.unique_banks = unique_bank_count;
  info.is_broadcast = all_same_address;
  info.exact = true;
  return info;
}

static std::optional<CoalescingInfo>
computeCoalescing(const AccessInfo &access, const LaunchDimensions &dims,
                  const DeviceConfig &config) {
  if (!access.address_pattern.valid || access.access_size == 0) {
    return std::nullopt;
  }

  const uint32_t lanes = getActiveLaneCount(dims, config);
  const uint32_t transaction_bytes = config.global_transaction_bytes;
  SmallVector<uint32_t, 32> segments;
  std::optional<int64_t> min_addr;
  std::optional<int64_t> max_addr;
  for (uint32_t lane = 0; lane < lanes; ++lane) {
    const LaneCoordinates coords = getLaneCoordinates(lane, dims);
    auto addr =
        evaluateAddressForThread(access.address_pattern, dims, coords.tid_x,
                                 coords.tid_y, coords.tid_z, 0, 0, 0);
    if (!addr) {
      return std::nullopt;
    }
    min_addr = !min_addr ? *addr : std::min(*min_addr, *addr);
    max_addr = !max_addr ? *addr : std::max(*max_addr, *addr);
    const uint32_t start = static_cast<uint32_t>(*addr / transaction_bytes);
    const uint32_t end = static_cast<uint32_t>(
        (*addr + access.access_size - 1) / transaction_bytes);
    for (uint32_t seg = start; seg <= end; ++seg) {
      if (std::find(segments.begin(), segments.end(), seg) == segments.end()) {
        segments.push_back(seg);
      }
    }
  }

  CoalescingInfo info;
  info.inst = access.inst;
  info.transaction_bytes = transaction_bytes;
  info.estimated_transactions = segments.size();
  info.participating_lanes = lanes;
  info.unique_segments = segments.size();
  if (min_addr && max_addr) {
    info.covered_bytes =
        static_cast<uint32_t>(*max_addr - *min_addr + access.access_size);
  }
  const uint32_t contiguous_bytes =
      lanes * std::max<uint32_t>(1, access.access_size);
  if (segments.size() <= 1 && info.covered_bytes <= transaction_bytes) {
    info.quality = CoalescingQuality::FullyCoalesced;
  } else if (segments.size() <= 2 &&
             info.covered_bytes <= contiguous_bytes * 2) {
    info.quality = CoalescingQuality::PartiallyCoalesced;
  } else {
    info.quality = CoalescingQuality::Uncoalesced;
  }
  return info;
}

} // namespace

static SynchronizationScope requiredOrderingScope(const AccessInfo &lhs,
                                                  const AccessInfo &rhs) {
  if (lhs.space == MemorySpace::Shared && rhs.space == MemorySpace::Shared) {
    return SynchronizationScope::Block;
  }
  return (lhs.depends_on_block_idx || rhs.depends_on_block_idx)
             ? SynchronizationScope::Device
             : SynchronizationScope::Block;
}

detail::AliasQueryResult detail::queryAlias(const AccessInfo &lhs,
                                            const AccessInfo &rhs,
                                            lotus::AliasAnalysisWrapper *aa) {
  detail::AliasQueryResult result;
  result.precision = std::max(lhs.alias_precision, rhs.alias_precision);
  result.source = lhs.alias_source == AliasSource::AserPTA ||
                          rhs.alias_source == AliasSource::AserPTA
                      ? AliasSource::AserPTA
                      : (lhs.alias_source == AliasSource::Wrapper ||
                                 rhs.alias_source == AliasSource::Wrapper
                             ? AliasSource::Wrapper
                             : AliasSource::Local);

  if (!aa || !aa->isInitialized() || !lhs.pointer || !rhs.pointer) {
    result.relation = sharesBaseObject(lhs, rhs) ? AliasResult::MayAlias
                                                 : AliasResult::NoAlias;
    return result;
  }

  result.relation = aa->query(lhs.pointer, rhs.pointer);
  result.source =
      aa->getConfig().impl == lotus::AAConfig::Implementation::AserPTA
          ? AliasSource::AserPTA
          : AliasSource::Wrapper;
  if (result.relation == AliasResult::NoAlias && sharesBaseObject(lhs, rhs)) {
    result.relation = AliasResult::MayAlias;
    result.precision = AliasPrecision::SymbolicAffine;
    return result;
  }
  if (result.relation == AliasResult::NoAlias ||
      result.relation == AliasResult::MustAlias) {
    result.precision = AliasPrecision::Exact;
  } else if (result.precision == AliasPrecision::Exact) {
    result.precision = AliasPrecision::SymbolicAffine;
  }
  return result;
}

void CUDAAnalysis::analyzeKernel(const Function *kernel,
                                 const KernelLaunchInfo *launch) {
  KernelSummary summary;
  summary.kernel = kernel;
  if (launch) {
    summary.dimensions = launch->dimensions;
  }
  for (unsigned i = 0; i < 3; ++i) {
    if (summary.dimensions.grid[i].kind == SymbolicValueKind::Unknown) {
      summary.dimensions.grid[i].kind = SymbolicValueKind::Constant;
      summary.dimensions.grid[i].constant = 1;
    }
    if (summary.dimensions.block[i].kind == SymbolicValueKind::Unknown) {
      summary.dimensions.block[i].kind = SymbolicValueKind::Constant;
      summary.dimensions.block[i].constant = 1;
    }
  }

  for (const Instruction &inst : instructions(*kernel)) {
    if (const auto *load = dyn_cast<LoadInst>(&inst)) {
      recordAccess(summary, &inst, load->getPointerOperand(), false);
    } else if (const auto *store = dyn_cast<StoreInst>(&inst)) {
      recordAccess(summary, &inst, store->getPointerOperand(), true);
    } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
      recordAccess(summary, &inst, rmw->getPointerOperand(), true);
      ++summary.atomic_count;
    } else if (const auto *cas = dyn_cast<AtomicCmpXchgInst>(&inst)) {
      recordAccess(summary, &inst, cas->getPointerOperand(), true);
      ++summary.atomic_count;
    } else if (const auto *call = dyn_cast<CallBase>(&inst)) {
      ThreadAPI::TD_TYPE type = m_thread_api->getType(call);
      if (type == ThreadAPI::TD_CUDA_ATOMIC) {
        ++summary.atomic_count;
      }
    }
  }

  analyzeDivergence(summary, kernel);
  analyzeWarpUniformity(summary, kernel);
  analyzeSynchronization(summary, kernel);
  analyzeRaces(summary);
  analyzeVolatile(summary);
  analyzeConstantAccesses(summary);

  m_kernel_index[kernel] = m_kernel_summaries.size();
  m_kernel_summaries.push_back(std::move(summary));
}

void CUDAAnalysis::recordAccess(KernelSummary &summary, const Instruction *inst,
                                const Value *pointer, bool is_write) {
  AccessInfo access;
  access.inst = inst;
  access.pointer = pointer;
  const BaseObjectInfo base_info = CUDAMemoryModel::getBaseObjectInfo(pointer);
  access.base_objects = base_info.objects;
  access.has_ambiguous_base = base_info.ambiguous;
  access.base = base_info.primary();
  const MemorySpaceInfo space_info = CUDAMemoryModel::classify(pointer);
  access.space = space_info.space;
  access.exact_space = space_info.exact;
  access.address_space = space_info.address_space;
  access.is_write = is_write;
  access.is_atomic = isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
  if (const auto *load = dyn_cast<LoadInst>(inst)) {
    access.is_volatile = load->isVolatile();
  } else if (const auto *store = dyn_cast<StoreInst>(inst)) {
    access.is_volatile = store->isVolatile();
  }
  access.depends_on_thread_idx = dependsOnThreadBuiltin(pointer);
  access.depends_on_block_idx = dependsOnBlockBuiltin(pointer);
  access.depends_on_lane_id = dependsOnLaneBuiltin(pointer);
  access.uniformity = classifyUniformity(pointer);
  access.participation = classifyParticipation(pointer);
  access.address_pattern = extractAffineAccessPattern(pointer);
  access.exact_address =
      access.address_pattern.valid && access.address_pattern.exact;
  access.alias_precision = getAliasPrecision(access);
  access.alias_source = AliasSource::Local;
  if (hasAliasAnalysis() && pointer && pointer->getType()->isPointerTy()) {
    std::vector<const Value *> pts;
    if (m_alias_analysis->getPointsToSet(pointer, pts) && !pts.empty()) {
      access.base_objects.clear();
      for (const Value *pt : pts) {
        if (pt) {
          access.base_objects.push_back(pt);
        }
      }
      access.base =
          access.base_objects.empty() ? nullptr : access.base_objects.front();
      access.has_ambiguous_base = access.base_objects.size() > 1;
      access.alias_precision = access.has_ambiguous_base
                                   ? AliasPrecision::Ambiguous
                                   : AliasPrecision::Exact;
      access.alias_source = m_alias_analysis->getConfig().impl ==
                                    lotus::AAConfig::Implementation::AserPTA
                                ? AliasSource::AserPTA
                                : AliasSource::Wrapper;
    }
  }
  if (const auto *call = dyn_cast<CallBase>(inst)) {
    access.ordering_scope = detail::getSyncScope(m_thread_api->getType(call));
    access.has_fence_relevance =
        access.ordering_scope != SynchronizationScope::None;
    access.fence_precedes = access.has_fence_relevance;
  }

  const DataLayout &dl = m_module.getDataLayout();
  if (const auto *load = dyn_cast<LoadInst>(inst)) {
    access.access_size =
        static_cast<uint32_t>(dl.getTypeStoreSize(load->getType()));
  } else if (const auto *store = dyn_cast<StoreInst>(inst)) {
    access.access_size = static_cast<uint32_t>(
        dl.getTypeStoreSize(store->getValueOperand()->getType()));
  } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst)) {
    access.access_size = static_cast<uint32_t>(
        dl.getTypeStoreSize(rmw->getValOperand()->getType()));
  } else if (const auto *cas = dyn_cast<AtomicCmpXchgInst>(inst)) {
    access.access_size = static_cast<uint32_t>(
        dl.getTypeStoreSize(cas->getCompareOperand()->getType()));
  }

  summary.accesses.push_back(access);

  CUDAAccessFact fact;
  fact.access_class_id = m_abstract_state.access_facts.size();
  fact.instruction = access.inst;
  fact.pointer = access.pointer;
  fact.base = access.base;
  fact.space = static_cast<int>(access.space);
  fact.is_write = access.is_write;
  fact.is_atomic = access.is_atomic;
  fact.uniformity = static_cast<int>(access.uniformity);
  m_abstract_state.access_facts.push_back(fact);
  m_abstract_state.access_fact_by_class[fact.access_class_id] = fact;

  switch (access.space) {
  case MemorySpace::Shared: {
    ++summary.shared_access_count;
    if (access.depends_on_thread_idx) {
      if (auto conflict = computeBankConflict(access, summary.dimensions,
                                              m_device_config)) {
        summary.has_bank_conflict = true;
        summary.bank_conflicts.push_back(*conflict);
      }
    }
    break;
  }
  case MemorySpace::Device:
    ++summary.device_access_count;
    [[fallthrough]];
  case MemorySpace::Global: {
    if (access.space == MemorySpace::Global) {
      ++summary.global_access_count;
    }
    if (access.depends_on_thread_idx) {
      if (auto info =
              computeCoalescing(access, summary.dimensions, m_device_config);
          info && (info->quality == CoalescingQuality::PartiallyCoalesced ||
                   info->quality == CoalescingQuality::Uncoalesced)) {
        summary.has_uncoalesced_access = true;
        summary.coalescing_issues.push_back(*info);
      }
    }
    break;
  }
  case MemorySpace::Constant:
    ++summary.constant_access_count;
    break;
  case MemorySpace::Local:
    ++summary.local_access_count;
    break;
  case MemorySpace::Host:
  case MemorySpace::Unknown:
    break;
  }
}

void CUDAAnalysis::analyzeDivergence(KernelSummary &summary,
                                     const Function *kernel) {
  if (!kernel || kernel->empty()) {
    return;
  }

  PostDominatorTree pdt;
  pdt.recalculate(const_cast<Function &>(*kernel));

  for (const BasicBlock &bb : *kernel) {
    const auto *branch = dyn_cast<BranchInst>(bb.getTerminator());
    if (!branch || !branch->isConditional()) {
      continue;
    }
    if (!dependsOnThreadBuiltin(branch->getCondition())) {
      continue;
    }
    if (isWarpUniformValue(branch->getCondition())) {
      continue;
    }

    DivergenceRegion region;
    region.branch = branch;
    region.depends_on_thread_idx =
        dependsOnThreadBuiltin(branch->getCondition());
    region.depends_on_block_idx = dependsOnBlockBuiltin(branch->getCondition());
    region.depends_on_lane_id = dependsOnLaneBuiltin(branch->getCondition());

    if (const DomTreeNodeBase<BasicBlock> *node = pdt.getNode(&bb)) {
      if (const DomTreeNodeBase<BasicBlock> *idom = node->getIDom()) {
        region.merge_block = idom->getBlock();
      }
    }
    if (!region.merge_block) {
      region.merge_block = pdt.findNearestCommonDominator(
          branch->getSuccessor(0), branch->getSuccessor(1));
    }

    for (unsigned succ_index = 0; succ_index < branch->getNumSuccessors();
         ++succ_index) {
      for (const BasicBlock *reachable : collectReachableBlocks(
               branch->getSuccessor(succ_index), region.merge_block)) {
        region.region_blocks.push_back(reachable);
        if (const Instruction *barrier =
                getFirstBarrierInBlock(reachable, m_thread_api)) {
          region.nested_barriers.push_back(barrier);
          summary.has_barrier_mismatch = true;
          summary.barrier_mismatches.push_back({branch, barrier});
        }
      }
    }

    summary.has_warp_divergence = true;
    summary.divergence_regions.push_back(std::move(region));
  }
}

static bool isOrderedBySynchronization(const AccessInfo &lhs,
                                       const AccessInfo &rhs,
                                       const KernelSummary &summary,
                                       SynchronizationScope required_scope) {
  for (const auto &sync : summary.synchronizations) {
    if (!sync.inst || !sync.orders_memory) {
      continue;
    }
    if (required_scope == SynchronizationScope::Device &&
        sync.scope != SynchronizationScope::Device &&
        sync.scope != SynchronizationScope::System) {
      continue;
    }
    if (required_scope == SynchronizationScope::Block &&
        sync.scope == SynchronizationScope::Warp &&
        (lhs.space == MemorySpace::Shared ||
         rhs.space == MemorySpace::Shared)) {
      continue;
    }
    if (!ordersMemorySpace(sync.primitive, lhs.space) ||
        !ordersMemorySpace(sync.primitive, rhs.space)) {
      continue;
    }

    bool lhs_before = lhs.inst &&
                      lhs.inst->getParent() == sync.inst->getParent() &&
                      comesBeforeInBlock(lhs.inst, sync.inst);
    bool rhs_after = rhs.inst &&
                     rhs.inst->getParent() == sync.inst->getParent() &&
                     comesBeforeInBlock(sync.inst, rhs.inst);

    if (!lhs_before) {
      for (const auto *pred : sync.preceding_blocks) {
        if (lhs.inst && lhs.inst->getParent() == pred) {
          lhs_before = true;
          break;
        }
      }
    }
    if (!rhs_after) {
      for (const auto *succ : sync.following_blocks) {
        if (rhs.inst && rhs.inst->getParent() == succ) {
          rhs_after = true;
          break;
        }
      }
    }
    if (!lhs_before || !rhs_after) {
      continue;
    }
    if (sync.execution_rendezvous) {
      if (sync.participation == ParticipationKind::Exact) {
        return true;
      }
      continue;
    }
    if (lhs.participation <= sync.participating_threads &&
        rhs.participation <= sync.participating_threads) {
      return true;
    }
  }
  return false;
}

static bool hasWarpOnlyOrderingBetween(const AccessInfo &lhs,
                                       const AccessInfo &rhs,
                                       const KernelSummary &summary) {
  for (const auto &sync : summary.synchronizations) {
    if (!sync.inst || sync.scope != SynchronizationScope::Warp) {
      continue;
    }

    bool lhs_before = lhs.inst &&
                      lhs.inst->getParent() == sync.inst->getParent() &&
                      comesBeforeInBlock(lhs.inst, sync.inst);
    bool rhs_after = rhs.inst &&
                     rhs.inst->getParent() == sync.inst->getParent() &&
                     comesBeforeInBlock(sync.inst, rhs.inst);

    if (!lhs_before) {
      for (const auto *pred : sync.preceding_blocks) {
        if (lhs.inst && lhs.inst->getParent() == pred) {
          lhs_before = true;
          break;
        }
      }
    }
    if (!rhs_after) {
      for (const auto *succ : sync.following_blocks) {
        if (rhs.inst && rhs.inst->getParent() == succ) {
          rhs_after = true;
          break;
        }
      }
    }
    if (lhs_before && rhs_after) {
      return true;
    }
  }
  return false;
}

static bool isUniformControlled(const AccessInfo &lhs, const AccessInfo &rhs,
                                const KernelSummary &summary) {
  if (lhs.participation > ParticipationScope::Warp ||
      rhs.participation > ParticipationScope::Warp) {
    return false;
  }
  for (const auto &uniform : summary.warp_uniform_regions) {
    if (!uniform.uniform_within_warp)
      continue;

    const Instruction *branch = uniform.branch;
    if (!branch)
      continue;

    if (!lhs.inst || !rhs.inst) {
      continue;
    }
    const BasicBlock *lhs_block = lhs.inst->getParent();
    const BasicBlock *rhs_block = rhs.inst->getParent();
    if (!lhs_block || !rhs_block || lhs_block == rhs_block) {
      continue;
    }
    if (!llvm::is_contained(uniform.uniform_blocks, lhs_block) ||
        !llvm::is_contained(uniform.uniform_blocks, rhs_block)) {
      continue;
    }
    if (!isReachable(lhs_block, rhs_block) &&
        !isReachable(rhs_block, lhs_block)) {
      return true;
    }
  }
  return false;
}

void CUDAAnalysis::analyzeRaces(KernelSummary &summary) {
  for (size_t i = 0; i < summary.accesses.size(); ++i) {
    for (size_t j = i + 1; j < summary.accesses.size(); ++j) {
      const AccessInfo &lhs = summary.accesses[i];
      const AccessInfo &rhs = summary.accesses[j];
      if (!(lhs.is_write || rhs.is_write)) {
        continue;
      }
      if (lhs.space != rhs.space && !((lhs.space == MemorySpace::Global ||
                                       lhs.space == MemorySpace::Device) &&
                                      (rhs.space == MemorySpace::Global ||
                                       rhs.space == MemorySpace::Device))) {
        continue;
      }

      const detail::AliasQueryResult alias =
          detail::queryAlias(lhs, rhs, m_alias_analysis);
      if (alias.relation == AliasResult::NoAlias) {
        continue;
      }

      if (lhs.space == MemorySpace::Shared &&
          isUniformControlled(lhs, rhs, summary)) {
        continue;
      }

      const SynchronizationScope required_scope =
          requiredOrderingScope(lhs, rhs);
      if (isOrderedBySynchronization(lhs, rhs, summary, required_scope)) {
        continue;
      }

      if (isSharedRaceRelevant(lhs) && isSharedRaceRelevant(rhs)) {
        RaceDecision decision = evaluateRaceDecision(
            lhs, rhs, summary.dimensions, m_device_config, false, alias);
        if (decision.aliases) {
          summary.has_shared_race = true;
          const bool warp_only_ordering =
              hasWarpOnlyOrderingBetween(lhs, rhs, summary);
          RaceInfo race;
          race.first = lhs.inst;
          race.second = rhs.inst;
          race.base = lhs.base;
          race.bases = mergeBaseObjects(lhs, rhs);
          race.space = MemorySpace::Shared;
          race.same_block_only = true;
          race.cross_block = false;
          race.symbolic = decision.symbolic;
          race.kind = warp_only_ordering ? RaceKind::MissingFence
                                         : ((lhs.is_atomic || rhs.is_atomic)
                                                ? RaceKind::AtomicOrderingRisk
                                                : RaceKind::DataRace);
          race.scope = SynchronizationScope::Block;
          race.ordering_reason =
              race.kind == RaceKind::MissingFence
                  ? "warp synchronization does not order block-wide shared "
                    "communication"
                  : ((lhs.is_atomic || rhs.is_atomic)
                         ? "atomic access is mixed with non-atomic access "
                           "without ordering"
                         : "missing block synchronization");
          race.ordering_inst = nullptr;
          race.required_fence_scope = race.kind == RaceKind::MissingFence
                                          ? SynchronizationScope::Block
                                          : SynchronizationScope::None;
          race.alias_precision = decision.precision;
          race.alias_source = decision.source;
          race.missing_ordering = required_scope == SynchronizationScope::Block
                                      ? SynchronizationPrimitive::BlockBarrier
                                      : SynchronizationPrimitive::DeviceFence;
          race.confidence = decision.confidence;
          race.exact =
              !race.symbolic && decision.precision == AliasPrecision::Exact;
          summary.shared_races.push_back(std::move(race));
        }
      } else if (isGlobalRaceRelevant(lhs) && isGlobalRaceRelevant(rhs)) {
        const bool cross_block =
            lhs.depends_on_block_idx || rhs.depends_on_block_idx;
        RaceDecision decision = evaluateRaceDecision(
            lhs, rhs, summary.dimensions, m_device_config, cross_block, alias);
        if (decision.aliases) {
          summary.has_global_race = true;
          RaceInfo race;
          race.first = lhs.inst;
          race.second = rhs.inst;
          race.base = lhs.base;
          race.bases = mergeBaseObjects(lhs, rhs);
          race.space = lhs.space;
          race.same_block_only = !cross_block;
          race.cross_block = cross_block;
          race.symbolic = decision.symbolic;
          race.kind = (lhs.is_atomic || rhs.is_atomic)
                          ? RaceKind::AtomicOrderingRisk
                          : RaceKind::MissingFence;
          race.scope = cross_block ? SynchronizationScope::Device
                                   : SynchronizationScope::Block;
          race.ordering_reason =
              (lhs.is_atomic || rhs.is_atomic)
                  ? "atomic/global communication requires stronger ordering"
                  : (cross_block ? "missing device ordering"
                                 : "missing intra-kernel fence or ordering");
          race.ordering_inst = nullptr;
          race.required_fence_scope = cross_block ? SynchronizationScope::Device
                                                  : SynchronizationScope::Block;
          race.alias_precision = decision.precision;
          race.alias_source = decision.source;
          race.missing_ordering = cross_block
                                      ? SynchronizationPrimitive::DeviceFence
                                      : SynchronizationPrimitive::BlockFence;
          race.confidence = decision.confidence;
          race.exact =
              !race.symbolic && decision.precision == AliasPrecision::Exact;
          summary.global_races.push_back(std::move(race));
        }
      }
    }
  }
}

void CUDAAnalysis::analyzeVolatile(KernelSummary &summary) {
  SmallPtrSet<const Value *, 8> has_volatile_base;
  for (const AccessInfo &access : summary.accesses) {
    if (access.is_volatile && access.base) {
      has_volatile_base.insert(access.base);
    }
  }

  SmallPtrSet<const Value *, 8> flagged_bases;
  for (const AccessInfo &access : summary.accesses) {
    if (access.is_atomic || access.is_volatile || !access.base) {
      continue;
    }
    if (access.space != MemorySpace::Shared &&
        access.space != MemorySpace::Global &&
        access.space != MemorySpace::Device) {
      continue;
    }
    if (!access.depends_on_thread_idx && !access.depends_on_block_idx) {
      continue;
    }
    if (has_volatile_base.count(access.base)) {
      continue;
    }
    if (!flagged_bases.insert(access.base).second) {
      continue;
    }
    summary.has_volatile_missing = true;
    summary.volatile_missing.push_back({access.inst, access.base});
  }
}

void CUDAAnalysis::analyzeWarpUniformity(KernelSummary &summary,
                                         const Function *kernel) {
  if (!kernel || kernel->empty()) {
    return;
  }

  PostDominatorTree pdt;
  pdt.recalculate(const_cast<Function &>(*kernel));

  for (const BasicBlock &bb : *kernel) {
    const auto *branch = dyn_cast<BranchInst>(bb.getTerminator());
    if (!branch || !branch->isConditional()) {
      continue;
    }

    const UniformityClass uniformity =
        classifyUniformity(branch->getCondition());
    bool uniform_within_warp = uniformity == UniformityClass::WarpUniform;
    bool uniform_within_block =
        uniform_within_warp || uniformity == UniformityClass::BlockUniform;

    if (uniform_within_warp || uniform_within_block) {
      WarpUniformInfo info;
      info.branch = branch;
      info.uniform_within_warp = uniform_within_warp;
      info.uniform_within_block = uniform_within_block;
      const BasicBlock *merge = pdt.findNearestCommonDominator(
          branch->getSuccessor(0), branch->getSuccessor(1));
      for (unsigned succ_index = 0; succ_index < branch->getNumSuccessors();
           ++succ_index) {
        for (const BasicBlock *reachable :
             collectReachableBlocks(branch->getSuccessor(succ_index), merge)) {
          if (!llvm::is_contained(info.uniform_blocks, reachable)) {
            info.uniform_blocks.push_back(reachable);
          }
        }
      }
      summary.warp_uniform_regions.push_back(std::move(info));
    }
  }
}

void CUDAAnalysis::analyzeSynchronization(KernelSummary &summary,
                                          const Function *kernel) {
  if (!kernel || kernel->empty()) {
    return;
  }

  for (const BasicBlock &bb : *kernel) {
    for (const Instruction &inst : bb) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      ThreadAPI::TD_TYPE type = m_thread_api->getType(call);
      if (type != ThreadAPI::TD_CUDA_BARRIER &&
          type != ThreadAPI::TD_CUDA_WARP_BARRIER &&
          type != ThreadAPI::TD_CUDA_MEMORY_BARRIER &&
          type != ThreadAPI::TD_CUDA_DEVICE_SYNC) {
        continue;
      }

      SynchronizationRecord info;
      info.inst = &inst;
      info.primitive = detail::getSynchronizationPrimitive(type, &inst);
      info.scope = detail::getSyncScope(type);
      info.orders_memory = info.scope != SynchronizationScope::None;
      info.execution_rendezvous = type == ThreadAPI::TD_CUDA_BARRIER ||
                                  type == ThreadAPI::TD_CUDA_WARP_BARRIER;
      info.participating_threads = type == ThreadAPI::TD_CUDA_WARP_BARRIER
                                       ? ParticipationScope::Warp
                                       : ParticipationScope::Block;

      for (const BasicBlock &candidate : *kernel) {
        if (&candidate == &bb) {
          continue;
        }
        if (isReachable(&candidate, &bb)) {
          info.preceding_blocks.push_back(&candidate);
        }
        if (isReachable(&bb, &candidate)) {
          info.following_blocks.push_back(&candidate);
        }
      }
      const bool all_threads_reach =
          info.execution_rendezvous ? mustReachBarrier(&inst, kernel) : false;
      info.participation = all_threads_reach ? ParticipationKind::Exact
                                             : ParticipationKind::Conditional;
      info.exact = all_threads_reach || !info.execution_rendezvous;
      summary.synchronizations.push_back(std::move(info));

      CUDASynchronizationFact fact;
      fact.synchronization_class_id =
          m_abstract_state.synchronization_facts.size();
      fact.inst = &inst;
      fact.primitive = static_cast<int>(info.primitive);
      fact.scope = static_cast<int>(info.scope);
      fact.ordering_effect = info.orders_memory;
      fact.participating_threads = static_cast<int>(info.participating_threads);
      m_abstract_state.synchronization_facts.push_back(fact);
      m_abstract_state
          .synchronization_fact_by_class[fact.synchronization_class_id] = fact;
    }
  }
}

MemorySpace CUDAAnalysis::classifyMemorySpace(const Value *value) {
  return CUDAMemoryModel::classify(value).space;
}

const char *CUDAAnalysis::toString(MemorySpace space) {
  switch (space) {
  case MemorySpace::Unknown:
    return "unknown";
  case MemorySpace::Host:
    return "host";
  case MemorySpace::Device:
    return "device";
  case MemorySpace::Local:
    return "local";
  case MemorySpace::Shared:
    return "shared";
  case MemorySpace::Global:
    return "global";
  case MemorySpace::Constant:
    return "constant";
  }
  return "unknown";
}

const char *CUDAAnalysis::toString(CoalescingQuality quality) {
  switch (quality) {
  case CoalescingQuality::Unknown:
    return "unknown";
  case CoalescingQuality::FullyCoalesced:
    return "fully-coalesced";
  case CoalescingQuality::PartiallyCoalesced:
    return "partially-coalesced";
  case CoalescingQuality::Uncoalesced:
    return "uncoalesced";
  }
  return "unknown";
}

const char *CUDAAnalysis::toString(UniformityClass uniformity) {
  switch (uniformity) {
  case UniformityClass::Unknown:
    return "unknown";
  case UniformityClass::WarpUniform:
    return "warp-uniform";
  case UniformityClass::BlockUniform:
    return "block-uniform";
  case UniformityClass::ThreadVarying:
    return "thread-varying";
  }
  return "unknown";
}

const char *CUDAAnalysis::toString(SynchronizationScope scope) {
  switch (scope) {
  case SynchronizationScope::None:
    return "none";
  case SynchronizationScope::Warp:
    return "warp";
  case SynchronizationScope::Block:
    return "block";
  case SynchronizationScope::Device:
    return "device";
  case SynchronizationScope::System:
    return "system";
  }
  return "none";
}

const char *CUDAAnalysis::toString(RaceKind kind) {
  switch (kind) {
  case RaceKind::DataRace:
    return "data-race";
  case RaceKind::AtomicOrderingRisk:
    return "atomic-ordering-risk";
  case RaceKind::MissingFence:
    return "missing-fence";
  case RaceKind::InterKernelHazard:
    return "inter-kernel-hazard";
  }
  return "data-race";
}

const char *CUDAAnalysis::toString(LaunchOrderingSource source) {
  switch (source) {
  case LaunchOrderingSource::None:
    return "none";
  case LaunchOrderingSource::DeviceSynchronize:
    return "device-sync";
  case LaunchOrderingSource::StreamSynchronize:
    return "stream-sync";
  case LaunchOrderingSource::MemoryBarrier:
    return "memory-barrier";
  case LaunchOrderingSource::ProgramOrder:
    return "program-order";
  case LaunchOrderingSource::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char *CUDAAnalysis::toString(AliasPrecision precision) {
  switch (precision) {
  case AliasPrecision::Exact:
    return "exact";
  case AliasPrecision::SymbolicAffine:
    return "symbolic-affine";
  case AliasPrecision::Ambiguous:
    return "ambiguous";
  case AliasPrecision::NonAffine:
    return "non-affine";
  }
  return "non-affine";
}

const char *CUDAAnalysis::toString(AliasSource source) {
  switch (source) {
  case AliasSource::Local:
    return "local";
  case AliasSource::AserPTA:
    return "aser-pta";
  case AliasSource::Wrapper:
    return "wrapper";
  }
  return "local";
}

const char *CUDAAnalysis::toString(SynchronizationPrimitive primitive) {
  switch (primitive) {
  case SynchronizationPrimitive::None:
    return "none";
  case SynchronizationPrimitive::WarpBarrier:
    return "warp-barrier";
  case SynchronizationPrimitive::BlockBarrier:
    return "block-barrier";
  case SynchronizationPrimitive::BlockFence:
    return "block-fence";
  case SynchronizationPrimitive::DeviceFence:
    return "device-fence";
  case SynchronizationPrimitive::SystemFence:
    return "system-fence";
  case SynchronizationPrimitive::DeviceSynchronize:
    return "device-synchronize";
  case SynchronizationPrimitive::StreamProgramOrder:
    return "stream-program-order";
  }
  return "none";
}

const Value *CUDAAnalysis::getMemoryOperand(const Instruction *inst) {
  if (const auto *load = dyn_cast<LoadInst>(inst)) {
    return load->getPointerOperand();
  }
  if (const auto *store = dyn_cast<StoreInst>(inst)) {
    return store->getPointerOperand();
  }
  if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst)) {
    return rmw->getPointerOperand();
  }
  if (const auto *cas = dyn_cast<AtomicCmpXchgInst>(inst)) {
    return cas->getPointerOperand();
  }
  return nullptr;
}

} // namespace concurrency::cuda
