#include "Concurrency/CUDA/CUDAAnalysis.h"

#include "Concurrency/CUDA/CUDAAnalysisHelpers.h"
#include "Concurrency/CUDA/CUDAParticipantAnalysis.h"

#include <algorithm>
#include <cmath>
#include <functional>
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
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace concurrency::cuda {

namespace {

static bool comesBeforeInBlock(const Instruction *lhs, const Instruction *rhs);

struct SyncControlInfo {
  DominatorTree dom_tree;
  PostDominatorTree post_dom_tree;
  bool valid = false;
};

struct RaceDecision {
  bool aliases = false;
  bool symbolic = false;
  AliasPrecision precision = AliasPrecision::NonAffine;
  AliasSource source = AliasSource::Local;
  double confidence = 0.0;
};

static bool isSharedRaceRelevant(const AccessInfo &access) {
  return access.space == MemorySpace::Shared;
}

static bool isGlobalRaceRelevant(const AccessInfo &access) {
  return (access.space == MemorySpace::Global ||
          access.space == MemorySpace::Device);
}

static bool isUnknownRaceRelevant(const AccessInfo &access) {
  return access.space == MemorySpace::Unknown || access.has_unresolved_base ||
         !access.base;
}

static bool mightBeManagedMemory(const AccessInfo &access) {
  if (!access.base) {
    return false;
  }
  if (const auto *gv = dyn_cast<GlobalValue>(access.base)) {
    StringRef name = gv->getName();
    if (name.contains("managed") || name.contains("Managed") ||
        name.contains("UM") || name.contains("um_")) {
      return true;
    }
    if (gv->hasSection() && gv->getSection().contains("managed")) {
      return true;
    }
  }
  return false;
}

static bool isCompatibleAtomicOrdering(AtomicOrdering lhs_order,
                                       AtomicOrdering rhs_order) {
  if ((lhs_order == AtomicOrdering::Release ||
       lhs_order == AtomicOrdering::AcquireRelease ||
       lhs_order == AtomicOrdering::SequentiallyConsistent) &&
      (rhs_order == AtomicOrdering::Acquire ||
       rhs_order == AtomicOrdering::AcquireRelease ||
       rhs_order == AtomicOrdering::SequentiallyConsistent)) {
    return true;
  }
  if (lhs_order == AtomicOrdering::SequentiallyConsistent &&
      rhs_order == AtomicOrdering::SequentiallyConsistent) {
    return true;
  }
  return false;
}

static bool isAtomicAccessOrdered(const AccessInfo &lhs, const AccessInfo &rhs,
                                  const KernelSummary &summary) {
  if (!(lhs.is_atomic && rhs.is_atomic)) {
    return false;
  }
  if (lhs.atomic_ordering == AtomicOrdering::SequentiallyConsistent &&
      rhs.atomic_ordering == AtomicOrdering::SequentiallyConsistent) {
    return true;
  }
  if (!isCompatibleAtomicOrdering(lhs.atomic_ordering, rhs.atomic_ordering)) {
    return false;
  }
  for (const auto &sync : summary.synchronizations) {
    if (!sync.orders_memory && !sync.orders_atomics) {
      continue;
    }
    if (sync.scope == SynchronizationScope::None) {
      continue;
    }
    bool lhs_before = lhs.inst &&
                      lhs.inst->getParent() == sync.inst->getParent() &&
                      comesBeforeInBlock(lhs.inst, sync.inst);
    bool rhs_after = rhs.inst &&
                     rhs.inst->getParent() == sync.inst->getParent() &&
                     comesBeforeInBlock(sync.inst, rhs.inst);
    if (lhs_before && rhs_after) {
      return true;
    }
  }
  return false;
}

static bool isConstantAddressAccess(const AccessInfo &access) {
  if (!access.address_pattern.valid) {
    return !access.depends_on_thread_idx && !access.depends_on_block_idx &&
           !access.depends_on_lane_id;
  }
  return access.address_pattern.thread_idx_x == 0 &&
         access.address_pattern.thread_idx_y == 0 &&
         access.address_pattern.thread_idx_z == 0 &&
         access.address_pattern.block_idx_x == 0 &&
         access.address_pattern.block_idx_y == 0 &&
         access.address_pattern.block_idx_z == 0 &&
         access.address_pattern.lane_id == 0;
}

static AliasPrecision getAliasPrecision(const AccessInfo &access) {
  if (access.has_ambiguous_base) {
    return AliasPrecision::Ambiguous;
  }
  if (access.address_pattern.non_affine) {
    return AliasPrecision::NonAffine;
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

static bool mustReachBarrier(const Instruction *barrier, const Function *kernel,
                             const PostDominatorTree *pdt = nullptr) {
  const BasicBlock *barrier_block = barrier ? barrier->getParent() : nullptr;
  const BasicBlock *entry =
      kernel && !kernel->empty() ? &kernel->getEntryBlock() : nullptr;
  if (!barrier_block || !entry) {
    return false;
  }
  if (pdt && pdt->getNode(barrier_block) &&
      pdt->dominates(barrier_block, entry)) {
    return true;
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

static bool doesBlockDominate(const DominatorTree *dt, const BasicBlock *lhs,
                              const BasicBlock *rhs) {
  return dt && lhs && rhs && dt->getNode(lhs) && dt->getNode(rhs) &&
         dt->dominates(lhs, rhs);
}

static bool isBeforeSynchronization(const AccessInfo &access,
                                    const SynchronizationRecord &sync,
                                    const SyncControlInfo &control) {
  if (!access.inst || !sync.inst) {
    return false;
  }
  const BasicBlock *access_block = access.inst->getParent();
  const BasicBlock *sync_block = sync.inst->getParent();
  if (!access_block || !sync_block) {
    return false;
  }
  if (access_block == sync_block) {
    return comesBeforeInBlock(access.inst, sync.inst);
  }
  return control.post_dom_tree.getNode(sync_block) &&
         control.post_dom_tree.getNode(access_block) &&
         control.post_dom_tree.dominates(sync_block, access_block);
}

static bool isAfterSynchronization(const AccessInfo &access,
                                   const SynchronizationRecord &sync,
                                   const SyncControlInfo &control) {
  if (!access.inst || !sync.inst) {
    return false;
  }
  const BasicBlock *access_block = access.inst->getParent();
  const BasicBlock *sync_block = sync.inst->getParent();
  if (!access_block || !sync_block) {
    return false;
  }
  if (access_block == sync_block) {
    return comesBeforeInBlock(sync.inst, access.inst);
  }
  return doesBlockDominate(&control.dom_tree, sync_block, access_block);
}

static bool isFullWarpMask(const CallBase *call) {
  if (!call || call->arg_empty()) {
    return false;
  }
  const auto mask =
      CUDASymbolicModel::evaluateConstantInt(call->getArgOperand(0));
  if (!mask) {
    return false;
  }
  return *mask == -1 || static_cast<uint64_t>(*mask) == 0xffffffffULL;
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

static std::optional<uint32_t> getBranchLaneMask(const BranchInst *branch,
                                                 bool take_true,
                                                 uint32_t warp_size) {
  const auto *compare = branch && branch->isConditional()
                            ? dyn_cast<ICmpInst>(branch->getCondition())
                            : nullptr;
  if (!compare || warp_size == 0 || warp_size > 32) {
    return std::nullopt;
  }
  const Value *builtin = compare->getOperand(0);
  const auto *constant = dyn_cast<ConstantInt>(compare->getOperand(1));
  ICmpInst::Predicate predicate = compare->getPredicate();
  if (!constant) {
    builtin = compare->getOperand(1);
    constant = dyn_cast<ConstantInt>(compare->getOperand(0));
    predicate = ICmpInst::getSwappedPredicate(predicate);
  }
  const BuiltinKind kind = CUDASymbolicModel::classifyBuiltin(builtin);
  if (!constant ||
      (kind != BuiltinKind::ThreadIdxX && kind != BuiltinKind::LaneId)) {
    return std::nullopt;
  }
  uint32_t mask = 0;
  const APInt rhs = constant->getValue();
  for (uint32_t lane = 0; lane < warp_size; ++lane) {
    const APInt lhs(rhs.getBitWidth(), lane);
    bool result = false;
    switch (predicate) {
    case ICmpInst::ICMP_EQ:
      result = lhs == rhs;
      break;
    case ICmpInst::ICMP_NE:
      result = lhs != rhs;
      break;
    case ICmpInst::ICMP_ULT:
      result = lhs.ult(rhs);
      break;
    case ICmpInst::ICMP_ULE:
      result = lhs.ule(rhs);
      break;
    case ICmpInst::ICMP_UGT:
      result = lhs.ugt(rhs);
      break;
    case ICmpInst::ICMP_UGE:
      result = lhs.uge(rhs);
      break;
    case ICmpInst::ICMP_SLT:
      result = lhs.slt(rhs);
      break;
    case ICmpInst::ICMP_SLE:
      result = lhs.sle(rhs);
      break;
    case ICmpInst::ICMP_SGT:
      result = lhs.sgt(rhs);
      break;
    case ICmpInst::ICMP_SGE:
      result = lhs.sge(rhs);
      break;
    default:
      return std::nullopt;
    }
    if (result == take_true) {
      mask |= uint32_t{1} << lane;
    }
  }
  return mask;
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

static std::array<int64_t, 3>
getCanonicalBlockDims(const LaunchDimensions &dims) {
  return {static_cast<int64_t>(getConcreteExtent(dims.block[0], 1)),
          static_cast<int64_t>(getConcreteExtent(dims.block[1], 1)),
          static_cast<int64_t>(getConcreteExtent(dims.block[2], 1))};
}

static std::array<int64_t, 3>
getCanonicalGridDims(const LaunchDimensions &dims) {
  return {static_cast<int64_t>(getConcreteExtent(dims.grid[0], 1)),
          static_cast<int64_t>(getConcreteExtent(dims.grid[1], 1)),
          static_cast<int64_t>(getConcreteExtent(dims.grid[2], 1))};
}

static std::optional<uint64_t>
getConcreteBlockCount(const LaunchDimensions &dims) {
  uint64_t blocks = 1;
  for (const SymbolicDimension &dim : dims.grid) {
    if (dim.kind != SymbolicValueKind::Constant) {
      return std::nullopt;
    }
    blocks *= dim.constant;
  }
  return blocks;
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
  const UniformityClass uniformity =
      CUDASymbolicModel::classifyUniformity(value);
  return uniformity == UniformityClass::WarpUniform ||
         uniformity == UniformityClass::BlockUniform;
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
  __int128 address = pattern.constant;
  address += static_cast<__int128>(pattern.thread_idx_x) * tid_x;
  address += static_cast<__int128>(pattern.thread_idx_y) * tid_y;
  address += static_cast<__int128>(pattern.thread_idx_z) * tid_z;
  address += static_cast<__int128>(pattern.block_idx_x) * block_x;
  address += static_cast<__int128>(pattern.block_idx_y) * block_y;
  address += static_cast<__int128>(pattern.block_idx_z) * block_z;
  address += static_cast<__int128>(pattern.lane_id) * (linear_tid % 32);
  if (address < std::numeric_limits<int64_t>::min() ||
      address > std::numeric_limits<int64_t>::max()) {
    return std::nullopt;
  }
  return static_cast<int64_t>(address);
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
  (void)config;
  if (!lhs.address_pattern.valid || !rhs.address_pattern.valid ||
      !lhs.address_pattern.exact || !rhs.address_pattern.exact) {
    return true;
  }

  if (dims.hasSymbolicBlock() ||
      (allow_cross_block && dims.hasSymbolicGrid())) {
    return true;
  }

  const uint32_t threads = getThreadsPerBlock(dims);
  const std::optional<uint64_t> concrete_blocks = getConcreteBlockCount(dims);
  if (!concrete_blocks || threads > 4096 || *concrete_blocks > 256) {
    return true;
  }
  const uint32_t block_count =
      allow_cross_block ? static_cast<uint32_t>(*concrete_blocks) : 1;
  const uint32_t grid_x = getConcreteExtent(dims.grid[0], 1);
  const uint32_t grid_y = getConcreteExtent(dims.grid[1], 1);

  for (uint32_t block_l = 0; block_l < block_count; ++block_l) {
    for (uint32_t block_r = 0; block_r < block_count; ++block_r) {
      if (!allow_cross_block && block_l != block_r) {
        continue;
      }
      for (uint32_t tid_l = 0; tid_l < threads; ++tid_l) {
        for (uint32_t tid_r = 0; tid_r < threads; ++tid_r) {
          if (tid_l == tid_r && block_l == block_r) {
            continue;
          }
          const LaneCoordinates lane_l = getLaneCoordinates(tid_l, dims);
          const LaneCoordinates lane_r = getLaneCoordinates(tid_r, dims);
          const int64_t block_l_x = block_l % grid_x;
          const int64_t block_l_y = (block_l / grid_x) % grid_y;
          const int64_t block_l_z = block_l / (grid_x * grid_y);
          const int64_t block_r_x = block_r % grid_x;
          const int64_t block_r_y = (block_r / grid_x) % grid_y;
          const int64_t block_r_z = block_r / (grid_x * grid_y);
          const auto lhs_addr = evaluateAddressForThread(
              lhs.address_pattern, dims, lane_l.tid_x, lane_l.tid_y,
              lane_l.tid_z, block_l_x, block_l_y, block_l_z);
          const auto rhs_addr = evaluateAddressForThread(
              rhs.address_pattern, dims, lane_r.tid_x, lane_r.tid_y,
              lane_r.tid_z, block_r_x, block_r_y, block_r_z);
          if (!lhs_addr || !rhs_addr) {
            return true;
          }
          const __int128 lhs_begin = *lhs_addr;
          const __int128 rhs_begin = *rhs_addr;
          const __int128 lhs_end =
              lhs_begin + std::max<uint32_t>(1, lhs.access_size);
          const __int128 rhs_end =
              rhs_begin + std::max<uint32_t>(1, rhs.access_size);
          if (lhs_begin < rhs_end && rhs_begin < lhs_end) {
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

  const auto canonical = CUDASymbolicModel::normalizeAffineAccessPattern(
      access.address_pattern, getCanonicalBlockDims(dims),
      getCanonicalGridDims(dims));
  if (!canonical.valid) {
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
      static_cast<uint32_t>(std::abs(canonical.thread_stride_bytes *
                                     std::max<int64_t>(1, access.access_size)));
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

  const auto canonical = CUDASymbolicModel::normalizeAffineAccessPattern(
      access.address_pattern, getCanonicalBlockDims(dims),
      getCanonicalGridDims(dims));
  if (!canonical.valid) {
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
  info.exact = !dims.hasSymbolicBlock() && access.exact_address;
  if (min_addr && max_addr) {
    info.covered_bytes =
        static_cast<uint32_t>(*max_addr - *min_addr + access.access_size);
  }
  const uint32_t per_lane_stride = std::max<uint32_t>(
      1, static_cast<uint32_t>(std::abs(canonical.thread_stride_bytes)));
  const uint32_t contiguous_bytes =
      lanes * std::max<uint32_t>(1, access.access_size) * per_lane_stride;
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
    const bool unresolved = lhs.has_unresolved_base ||
                            rhs.has_unresolved_base || lhs.has_ambiguous_base ||
                            rhs.has_ambiguous_base || !lhs.base || !rhs.base ||
                            isa<Argument>(lhs.base) || isa<Argument>(rhs.base);
    if (sharesBaseObject(lhs, rhs) || unresolved) {
      result.relation = AliasResult::MayAlias;
    } else {
      const bool distinct_allocations =
          (isa<AllocaInst>(lhs.base) || isa<GlobalValue>(lhs.base)) &&
          (isa<AllocaInst>(rhs.base) || isa<GlobalValue>(rhs.base));
      result.relation =
          distinct_allocations ? AliasResult::NoAlias : AliasResult::MayAlias;
    }
    return result;
  }

  result.relation = aa->query(lhs.pointer, rhs.pointer);
  result.source =
      aa->getConfig().impl == lotus::AAConfig::Implementation::AserPTA
          ? AliasSource::AserPTA
          : AliasSource::Wrapper;
  const bool cannot_trust_disjoint_bases =
      lhs.has_unresolved_base || rhs.has_unresolved_base ||
      lhs.has_ambiguous_base || rhs.has_ambiguous_base ||
      isa_and_nonnull<Argument>(lhs.base) ||
      isa_and_nonnull<Argument>(rhs.base);
  if (result.relation == AliasResult::NoAlias &&
      (sharesBaseObject(lhs, rhs) || cannot_trust_disjoint_bases)) {
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
  summary.has_symbolic_grid = summary.dimensions.hasSymbolicGrid();
  summary.has_symbolic_block = summary.dimensions.hasSymbolicBlock();

  using ArgumentMap = DenseMap<const Value *, const Value *>;
  SmallPtrSet<const Function *, 8> active_functions;
  SetVector<const Function *> reachable_functions;
  std::function<void(const Function *, const ArgumentMap &)> scan_function;
  scan_function = [&](const Function *function, const ArgumentMap &arguments) {
    if (!function || function->isDeclaration() ||
        !active_functions.insert(function).second) {
      if (function && active_functions.count(function)) {
        recordModelGap(function,
                       "Recursive device call memory effects were "
                       "conservatively truncated",
                       0.4);
      }
      return;
    }
    reachable_functions.insert(function);

    auto resolve_actual = [&](const Value *value) -> const Value * {
      const BaseObjectInfo info = CUDAMemoryModel::getBaseObjectInfo(value);
      const Value *base = info.primary();
      auto mapped = arguments.find(base);
      return mapped == arguments.end() ? value : mapped->second;
    };

    auto record_mapped_access = [&](const Instruction *inst,
                                    const Value *pointer, bool is_write) {
      const size_t old_size = summary.accesses.size();
      recordAccess(summary, inst, pointer, is_write);
      if (summary.accesses.size() == old_size) {
        return;
      }
      const BaseObjectInfo formal_info =
          CUDAMemoryModel::getBaseObjectInfo(pointer);
      auto mapped = arguments.find(formal_info.primary());
      if (mapped == arguments.end()) {
        return;
      }

      AccessInfo &access = summary.accesses.back();
      const Value *actual = mapped->second;
      const BaseObjectInfo actual_info =
          CUDAMemoryModel::getBaseObjectInfo(actual);
      access.base_objects = actual_info.objects;
      access.has_ambiguous_base = actual_info.ambiguous;
      access.has_unresolved_base = actual_info.unresolved;
      access.base = actual_info.primary();
      access.pointer = actual;
      const MemorySpaceInfo actual_space = CUDAMemoryModel::classify(actual);
      access.space = actual_space.space;
      access.exact_space = actual_space.exact;
      access.address_space = actual_space.address_space;
      access.alias_precision = actual_info.ambiguous || actual_info.unresolved
                                   ? AliasPrecision::Ambiguous
                                   : std::max(access.alias_precision,
                                              AliasPrecision::SymbolicAffine);
      if (!m_abstract_state.access_facts.empty()) {
        CUDAAccessFact &fact = m_abstract_state.access_facts.back();
        fact.pointer = access.pointer;
        fact.base = access.base;
        fact.space = static_cast<int>(access.space);
        m_abstract_state.access_fact_by_class[fact.access_class_id] = fact;
      }
    };

    for (const Instruction &inst : instructions(*function)) {
      if (const auto *load = dyn_cast<LoadInst>(&inst)) {
        record_mapped_access(&inst, load->getPointerOperand(), false);
      } else if (const auto *store = dyn_cast<StoreInst>(&inst)) {
        record_mapped_access(&inst, store->getPointerOperand(), true);
      } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
        record_mapped_access(&inst, rmw->getPointerOperand(), true);
        ++summary.atomic_count;
      } else if (const auto *cas = dyn_cast<AtomicCmpXchgInst>(&inst)) {
        record_mapped_access(&inst, cas->getPointerOperand(), true);
        ++summary.atomic_count;
      } else if (const auto *transfer = dyn_cast<MemTransferInst>(&inst)) {
        record_mapped_access(&inst, transfer->getSource(), false);
        record_mapped_access(&inst, transfer->getDest(), true);
      } else if (const auto *set = dyn_cast<MemSetInst>(&inst)) {
        record_mapped_access(&inst, set->getDest(), true);
      } else if (const auto *call = dyn_cast<CallBase>(&inst)) {
        ThreadAPI::TD_TYPE type = m_thread_api->getType(call);
        if (type == ThreadAPI::TD_CUDA_ATOMIC) {
          ++summary.atomic_count;
          for (const Value *argument : call->args()) {
            if (!argument->getType()->isPointerTy()) {
              continue;
            }
            const size_t old_size = summary.accesses.size();
            record_mapped_access(&inst, argument, true);
            if (summary.accesses.size() != old_size) {
              summary.accesses.back().is_atomic = true;
              summary.accesses.back().atomic_ordering =
                  AtomicOrdering::SequentiallyConsistent;
              if (!m_abstract_state.access_facts.empty()) {
                CUDAAccessFact &fact = m_abstract_state.access_facts.back();
                fact.is_atomic = true;
                m_abstract_state.access_fact_by_class[fact.access_class_id] =
                    fact;
              }
            }
            break;
          }
          continue;
        }
        const Function *callee = call->getCalledFunction();
        if (!callee || callee->isDeclaration() || callee->isIntrinsic()) {
          continue;
        }
        ArgumentMap callee_arguments;
        unsigned index = 0;
        for (const Argument &formal : callee->args()) {
          if (index >= call->arg_size()) {
            break;
          }
          callee_arguments[&formal] =
              resolve_actual(call->getArgOperand(index++));
        }
        scan_function(callee, callee_arguments);
      }
    }
    active_functions.erase(function);
  };

  scan_function(kernel, ArgumentMap{});

  for (const Function *function : reachable_functions) {
    analyzeDivergence(summary, function);
    analyzeWarpUniformity(summary, function);
    const size_t sync_begin = summary.synchronizations.size();
    analyzeSynchronization(summary, function);
    if (function != kernel) {
      for (size_t index = sync_begin; index < summary.synchronizations.size();
           ++index) {
        summary.synchronizations[index].participation =
            ParticipationKind::Conditional;
        summary.synchronizations[index].exact = false;
      }
    }
  }
  auto is_divergently_executed = [&](const Instruction *inst) {
    if (!inst) {
      return false;
    }
    for (const DivergenceRegion &region : summary.divergence_regions) {
      if (llvm::is_contained(region.region_blocks, inst->getParent())) {
        return true;
      }
    }
    return false;
  };
  for (BankConflictInfo &conflict : summary.bank_conflicts) {
    if (is_divergently_executed(conflict.inst)) {
      conflict.exact = false;
      recordModelGap(conflict.inst,
                     "Bank-conflict estimate has a "
                     "path-dependent active-lane mask",
                     0.5);
    }
  }
  for (CoalescingInfo &coalescing : summary.coalescing_issues) {
    if (is_divergently_executed(coalescing.inst)) {
      coalescing.exact = false;
      recordModelGap(coalescing.inst,
                     "Coalescing estimate has a "
                     "path-dependent active-lane mask",
                     0.5);
    }
  }
  analyzeRaces(summary);
  analyzeVolatile(summary);
  analyzeConstantAccesses(summary);
  analyzeTextureAndSurfaceAccesses(summary);

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
  access.has_unresolved_base = base_info.unresolved;
  access.base = base_info.primary();
  const MemorySpaceInfo space_info = CUDAMemoryModel::classify(pointer);
  access.space = space_info.space;
  access.exact_space = space_info.exact;
  access.address_space = space_info.address_space;
  access.is_write = is_write;
  access.is_atomic = isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
  if (const auto *load = dyn_cast<LoadInst>(inst)) {
    access.is_volatile = load->isVolatile();
    access.atomic_ordering = load->getOrdering();
  } else if (const auto *store = dyn_cast<StoreInst>(inst)) {
    access.is_volatile = store->isVolatile();
    access.atomic_ordering = store->getOrdering();
  } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst)) {
    access.atomic_ordering = rmw->getOrdering();
  } else if (const auto *cas = dyn_cast<AtomicCmpXchgInst>(inst)) {
    access.atomic_ordering = cas->getSuccessOrdering();
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
  if (!access.address_pattern.valid || access.address_pattern.non_affine) {
    access.alias_precision = AliasPrecision::NonAffine;
  }
  if (!access.base || access.space == MemorySpace::Unknown ||
      !access.address_pattern.valid) {
    std::string msg = "CUDA access lost precision";
    if (!access.base && access.space == MemorySpace::Unknown) {
      msg += ": unresolved base object and unknown memory space";
    } else if (!access.base) {
      msg += ": unresolved base object";
    } else if (access.space == MemorySpace::Unknown) {
      msg += ": unknown memory space";
    } else {
      msg += ": non-affine address expression";
    }
    recordModelGap(inst, msg, access.address_pattern.valid ? 0.45 : 0.35);
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
  } else if (const auto *memory = dyn_cast<MemIntrinsic>(inst)) {
    if (const auto length = evaluateConstantInt(memory->getLength())) {
      access.access_size = static_cast<uint32_t>(std::max<int64_t>(0, *length));
    }
  } else if (const auto *call = dyn_cast<CallBase>(inst)) {
    for (const Value *argument : call->args()) {
      if (argument->getType()->isPointerTy() ||
          !argument->getType()->isSized()) {
        continue;
      }
      access.access_size =
          static_cast<uint32_t>(dl.getTypeStoreSize(argument->getType()));
      break;
    }
    if (access.access_size == 0) {
      access.access_size = 1;
    }
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
  case MemorySpace::ClusterShared:
    recordModelGap(inst,
                   "Cluster-shared access requires cluster-scope "
                   "synchronization modeling",
                   0.45);
    break;
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
          const auto *call = dyn_cast<CallBase>(barrier);
          const bool is_warp_barrier =
              call &&
              m_thread_api->getType(call) == ThreadAPI::TD_CUDA_WARP_BARRIER;
          std::optional<uint32_t> barrier_mask;
          if (is_warp_barrier && !call->arg_empty()) {
            if (const auto *mask =
                    dyn_cast<ConstantInt>(call->getArgOperand(0))) {
              const APInt full = APInt::getLowBitsSet(
                  mask->getBitWidth(),
                  std::min<unsigned>(m_device_config.warp_size,
                                     mask->getBitWidth()));
              if (!mask->isZero() && mask->getValue() != full) {
                barrier_mask = static_cast<uint32_t>(
                    mask->getValue().zextOrTrunc(32).getZExtValue());
              }
            }
          }
          if (is_warp_barrier && m_device_config.sm_major >= 7 &&
              barrier_mask) {
            const auto branch_mask = getBranchLaneMask(
                branch, succ_index == 0, m_device_config.warp_size);
            if (branch_mask && *branch_mask == *barrier_mask) {
              continue;
            }
            if (!branch_mask) {
              recordModelGap(
                  barrier,
                  "Masked warp barrier requires path-predicate/mask validation",
                  0.55);
              continue;
            }
          }
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
                                       SynchronizationScope required_scope,
                                       const SyncControlInfo &control) {
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

    const bool lhs_before = isBeforeSynchronization(lhs, sync, control);
    const bool rhs_after = isAfterSynchronization(rhs, sync, control);
    if (!lhs_before || !rhs_after) {
      continue;
    }
    if (sync.execution_rendezvous &&
        sync.participation == ParticipationKind::Exact) {
      return true;
    }
  }
  return false;
}

static bool hasWarpOnlyOrderingBetween(const AccessInfo &lhs,
                                       const AccessInfo &rhs,
                                       const KernelSummary &summary,
                                       const SyncControlInfo &control) {
  for (const auto &sync : summary.synchronizations) {
    if (!sync.inst || sync.scope != SynchronizationScope::Warp) {
      continue;
    }
    if (isBeforeSynchronization(lhs, sync, control) &&
        isAfterSynchronization(rhs, sync, control)) {
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

static bool isSingleThreadGuarded(const AccessInfo &access,
                                  const LaunchDimensions &dimensions) {
  if (!access.inst || !access.inst->getFunction() ||
      dimensions.hasSymbolicBlock() ||
      getConcreteExtent(dimensions.block[1], 1) != 1 ||
      getConcreteExtent(dimensions.block[2], 1) != 1) {
    return false;
  }
  DominatorTree dom_tree(*const_cast<Function *>(access.inst->getFunction()));
  for (const BasicBlock &block : *access.inst->getFunction()) {
    const auto *branch = dyn_cast<BranchInst>(block.getTerminator());
    const auto *compare = branch && branch->isConditional()
                              ? dyn_cast<ICmpInst>(branch->getCondition())
                              : nullptr;
    if (!compare || !dom_tree.dominates(branch, access.inst)) {
      continue;
    }
    const Value *builtin = compare->getOperand(0);
    const auto *constant = dyn_cast<ConstantInt>(compare->getOperand(1));
    if (!constant) {
      builtin = compare->getOperand(1);
      constant = dyn_cast<ConstantInt>(compare->getOperand(0));
    }
    if (!constant || CUDASymbolicModel::classifyBuiltin(builtin) !=
                         BuiltinKind::ThreadIdxX) {
      continue;
    }
    const bool in_true =
        dom_tree.dominates(branch->getSuccessor(0), access.inst->getParent());
    const bool in_false =
        dom_tree.dominates(branch->getSuccessor(1), access.inst->getParent());
    if (in_true == in_false) {
      continue;
    }
    const ICmpInst::Predicate predicate = compare->getPredicate();
    if ((in_true && predicate == ICmpInst::ICMP_EQ) ||
        (in_false && predicate == ICmpInst::ICMP_NE) ||
        (in_true && predicate == ICmpInst::ICMP_ULT &&
         constant->equalsInt(1)) ||
        (in_true && predicate == ICmpInst::ICMP_ULE && constant->isZero())) {
      return true;
    }
  }
  return false;
}

void CUDAAnalysis::recordModelGap(const Instruction *inst, StringRef message,
                                  double confidence) {
  CUDAModelGap gap;
  gap.gap_class_id = m_abstract_state.model_gaps.size();
  gap.explanation = message.str();
  gap.confidence = confidence;
  if (inst) {
    gap.related_instructions.push_back(inst);
  }
  m_abstract_state.model_gaps.push_back(gap);
  m_abstract_state.model_gap_by_class[gap.gap_class_id] = gap;
}

void CUDAAnalysis::recordModelGap(const Function *function, StringRef message,
                                  double confidence) {
  const Instruction *anchor = nullptr;
  if (function && !function->empty()) {
    anchor = &*function->getEntryBlock().begin();
  }
  recordModelGap(anchor, message, confidence);
}

void CUDAAnalysis::analyzeRaces(KernelSummary &summary) {
  SyncControlInfo control;
  if (summary.kernel && !summary.kernel->empty()) {
    control.dom_tree.recalculate(const_cast<Function &>(*summary.kernel));
    control.post_dom_tree.recalculate(const_cast<Function &>(*summary.kernel));
    control.valid = true;
  }
  const std::optional<uint64_t> concrete_block_count =
      getConcreteBlockCount(summary.dimensions);
  const bool known_multi_block =
      concrete_block_count && *concrete_block_count > 1;
  const bool can_have_multiple_blocks =
      summary.has_symbolic_grid || known_multi_block;

  const bool can_have_multiple_threads =
      summary.has_symbolic_block || getThreadsPerBlock(summary.dimensions) > 1;
  for (const AccessInfo &access : summary.accesses) {
    if (!access.is_write || access.is_atomic ||
        (!isSharedRaceRelevant(access) && !isGlobalRaceRelevant(access) &&
         !isUnknownRaceRelevant(access))) {
      continue;
    }

    const bool report_cross_block_only =
        can_have_multiple_blocks &&
        (isGlobalRaceRelevant(access) || isUnknownRaceRelevant(access));
    if (can_have_multiple_threads && !report_cross_block_only &&
        !isSingleThreadGuarded(access, summary.dimensions) &&
        hasDistinctThreadAlias(access, access, summary.dimensions,
                               m_device_config, false)) {
      RaceInfo race;
      race.first = access.inst;
      race.second = access.inst;
      race.base = access.base;
      race.bases = access.base_objects;
      race.space = access.space;
      race.same_block_only = true;
      race.symbolic = summary.has_symbolic_block || !access.exact_address;
      race.scope = SynchronizationScope::Block;
      race.ordering_reason =
          "distinct threads may execute the same static write access";
      race.alias_precision = access.alias_precision;
      race.alias_source = access.alias_source;
      race.confidence = access.exact_address ? 0.9 : 0.45;
      race.exact = access.exact_address && !summary.has_symbolic_block;
      if (isSharedRaceRelevant(access)) {
        summary.has_shared_race = true;
        summary.shared_races.push_back(std::move(race));
      } else {
        summary.has_global_race = true;
        summary.global_races.push_back(std::move(race));
      }
    }

    if ((!isGlobalRaceRelevant(access) && !isUnknownRaceRelevant(access)) ||
        !can_have_multiple_blocks ||
        !hasDistinctThreadAlias(access, access, summary.dimensions,
                                m_device_config, true)) {
      continue;
    }
    summary.has_global_race = true;
    RaceInfo race;
    race.first = access.inst;
    race.second = access.inst;
    race.base = access.base;
    race.bases = access.base_objects;
    race.space = access.space;
    race.cross_block = true;
    race.symbolic = summary.has_symbolic_grid;
    race.scope = SynchronizationScope::Device;
    race.ordering_reason =
        "distinct blocks may execute overlapping instances of this access";
    race.required_fence_scope = SynchronizationScope::Device;
    race.alias_precision = access.alias_precision;
    race.alias_source = access.alias_source;
    race.missing_ordering = SynchronizationPrimitive::DeviceFence;
    race.confidence = access.exact_address ? 0.9 : 0.4;
    race.exact =
        known_multi_block && !summary.has_symbolic_grid && access.exact_address;
    summary.global_races.push_back(std::move(race));
  }

  for (size_t i = 0; i < summary.accesses.size(); ++i) {
    for (size_t j = i + 1; j < summary.accesses.size(); ++j) {
      const AccessInfo &lhs = summary.accesses[i];
      const AccessInfo &rhs = summary.accesses[j];
      if (!(lhs.is_write || rhs.is_write)) {
        continue;
      }
      const detail::AliasQueryResult alias =
          detail::queryAlias(lhs, rhs, m_alias_analysis);
      if (alias.relation == AliasResult::NoAlias) {
        continue;
      }

      if (alias.precision == AliasPrecision::Ambiguous ||
          alias.precision == AliasPrecision::NonAffine) {
        const Instruction *inst = lhs.inst ? lhs.inst : rhs.inst;
        recordModelGap(inst,
                       "Race check is proceeding with imprecise alias "
                       "or non-affine address information",
                       0.4);
      }

      if (lhs.space == MemorySpace::Shared) {
        if (isUniformControlled(lhs, rhs, summary) &&
            lhs.participation < ParticipationScope::Warp &&
            rhs.participation < ParticipationScope::Warp) {
        }
      }

      if (isGlobalRaceRelevant(lhs) && isGlobalRaceRelevant(rhs)) {
        const bool syntactic_block_dep =
            lhs.depends_on_block_idx || rhs.depends_on_block_idx;
        const bool can_cross = summary.has_symbolic_grid || known_multi_block;
        if (can_cross && !syntactic_block_dep) {
          summary.has_global_race = true;
          RaceInfo race;
          race.first = lhs.inst;
          race.second = rhs.inst;
          race.base = lhs.base;
          race.space = MemorySpace::Global;
          race.same_block_only = false;
          race.cross_block = true;
          race.symbolic = summary.has_symbolic_grid;
          race.kind = RaceKind::DataRace;
          race.scope = SynchronizationScope::Device;
          race.confidence = 0.75;
          race.exact = known_multi_block && !summary.has_symbolic_grid;
          race.ordering_reason = "multi-block constant address";
          summary.global_races.push_back(race);
          continue;
        }
      }

      if (isUnknownRaceRelevant(lhs) || isUnknownRaceRelevant(rhs)) {
        RaceDecision decision =
            evaluateRaceDecision(lhs, rhs, summary.dimensions, m_device_config,
                                 can_have_multiple_blocks, alias);
        if (!decision.aliases) {
          continue;
        }
        summary.has_global_race = true;
        RaceInfo race;
        race.first = lhs.inst;
        race.second = rhs.inst;
        race.base = lhs.base ? lhs.base : rhs.base;
        race.bases = mergeBaseObjects(lhs, rhs);
        race.space = MemorySpace::Unknown;
        race.same_block_only = !can_have_multiple_blocks;
        race.cross_block = can_have_multiple_blocks;
        race.symbolic = true;
        race.kind = lhs.is_atomic || rhs.is_atomic
                        ? RaceKind::AtomicOrderingRisk
                        : RaceKind::DataRace;
        race.scope = can_have_multiple_blocks ? SynchronizationScope::Device
                                              : SynchronizationScope::Block;
        race.ordering_reason = "unknown CUDA memory effect may overlap";
        race.alias_precision = AliasPrecision::Ambiguous;
        race.alias_source = decision.source;
        race.confidence = 0.3;
        summary.global_races.push_back(std::move(race));
      } else if (isSharedRaceRelevant(lhs) && isSharedRaceRelevant(rhs)) {
        RaceDecision decision = evaluateRaceDecision(
            lhs, rhs, summary.dimensions, m_device_config, false, alias);
        if (decision.aliases) {
          if (isOrderedBySynchronization(
                  lhs, rhs, summary, SynchronizationScope::Block, control) ||
              isOrderedBySynchronization(
                  rhs, lhs, summary, SynchronizationScope::Block, control)) {
            continue;
          }
          if (lhs.is_atomic && rhs.is_atomic &&
              isAtomicAccessOrdered(lhs, rhs, summary)) {
            continue;
          }
          summary.has_shared_race = true;
          const bool warp_only_ordering =
              hasWarpOnlyOrderingBetween(lhs, rhs, summary, control);
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
                                         : (((lhs.is_atomic || rhs.is_atomic) &&
                                             !(lhs.is_atomic && rhs.is_atomic))
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
          race.missing_ordering = SynchronizationPrimitive::DeviceFence;
          race.confidence = decision.confidence;
          race.exact =
              !race.symbolic && decision.precision == AliasPrecision::Exact;
          summary.shared_races.push_back(std::move(race));
        }
      } else if (isGlobalRaceRelevant(lhs) && isGlobalRaceRelevant(rhs)) {
        const bool syntactic_block_dep =
            lhs.depends_on_block_idx || rhs.depends_on_block_idx;
        const bool constant_address =
            isConstantAddressAccess(lhs) && isConstantAddressAccess(rhs);
        const bool cross_block =
            syntactic_block_dep || can_have_multiple_blocks;
        if (cross_block && constant_address &&
            (can_have_multiple_blocks || syntactic_block_dep)) {
          summary.has_global_race = true;
          RaceInfo race;
          race.first = lhs.inst;
          race.second = rhs.inst;
          race.base = lhs.base;
          race.space = MemorySpace::Global;
          race.same_block_only = false;
          race.cross_block = true;
          race.symbolic = summary.has_symbolic_grid;
          race.kind = RaceKind::DataRace;
          race.scope = SynchronizationScope::Device;
          race.confidence = 0.7;
          race.exact = known_multi_block && !summary.has_symbolic_grid;
          race.ordering_reason =
              "multiple blocks may execute, constant address";
          summary.global_races.push_back(std::move(race));
          continue;
        }
        RaceDecision decision = evaluateRaceDecision(
            lhs, rhs, summary.dimensions, m_device_config, cross_block, alias);
        if (decision.aliases) {
          const SynchronizationScope required_scope =
              cross_block ? SynchronizationScope::Device
                          : SynchronizationScope::Block;
          if (isOrderedBySynchronization(lhs, rhs, summary, required_scope,
                                         control) ||
              isOrderedBySynchronization(rhs, lhs, summary, required_scope,
                                         control)) {
            continue;
          }
          if (lhs.is_atomic && rhs.is_atomic &&
              isAtomicAccessOrdered(lhs, rhs, summary)) {
            continue;
          }
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
                          ? ((lhs.is_atomic && rhs.is_atomic)
                                 ? RaceKind::MissingFence
                                 : RaceKind::AtomicOrderingRisk)
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
  // Volatile is not a general CUDA race-prevention requirement. Diagnose it
  // only when a future protocol recognizer identifies a communication idiom
  // whose correctness specifically relies on volatile accesses.
  summary.has_volatile_missing = false;
  summary.volatile_missing.clear();
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
    bool uniform_within_warp = uniformity == UniformityClass::WarpUniform ||
                               uniformity == UniformityClass::BlockUniform;
    bool uniform_within_block = uniformity == UniformityClass::BlockUniform;

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

  SyncControlInfo control;
  control.dom_tree.recalculate(const_cast<Function &>(*kernel));
  control.post_dom_tree.recalculate(const_cast<Function &>(*kernel));
  control.valid = true;
  CUDAParticipantAnalysis participant_analysis(*kernel,
                                               m_device_config.warp_size);

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
      info.orders_atomics =
          info.orders_memory || type == ThreadAPI::TD_CUDA_DEVICE_SYNC;
      info.execution_rendezvous = type == ThreadAPI::TD_CUDA_BARRIER ||
                                  type == ThreadAPI::TD_CUDA_WARP_BARRIER;
      info.participating_threads =
          type == ThreadAPI::TD_CUDA_WARP_BARRIER
              ? ParticipationScope::Warp
              : (type == ThreadAPI::TD_CUDA_BARRIER ? ParticipationScope::Block
                                                    : ParticipationScope::Grid);

      if (info.execution_rendezvous) {
        const CUDAParticipantSet participants =
            participant_analysis.getActiveParticipants(&inst);
        switch (participants.certainty) {
        case ParticipantCertainty::Exact:
          info.participation = ParticipationKind::Exact;
          break;
        case ParticipantCertainty::Partial:
          info.participation = ParticipationKind::Partial;
          break;
        case ParticipantCertainty::Conditional:
        case ParticipantCertainty::Unknown:
          info.participation = ParticipationKind::Conditional;
          break;
        }
        info.exact = participants.certainty == ParticipantCertainty::Exact;
      }

      for (const BasicBlock &candidate : *kernel) {
        if (&candidate == &bb) {
          continue;
        }
        if (doesBlockDominate(&control.dom_tree, &candidate, &bb)) {
          info.preceding_blocks.push_back(&candidate);
        }
        if (doesBlockDominate(&control.dom_tree, &bb, &candidate)) {
          info.following_blocks.push_back(&candidate);
        }
      }
      if (!info.execution_rendezvous) {
        const bool all_threads_reach =
            info.execution_rendezvous
                ? mustReachBarrier(&inst, kernel, &control.post_dom_tree)
                : false;
        info.participation = all_threads_reach ? ParticipationKind::Exact
                                               : ParticipationKind::Conditional;
        info.exact = all_threads_reach || !info.execution_rendezvous;
      }
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
  case MemorySpace::ClusterShared:
    return "cluster-shared";
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
