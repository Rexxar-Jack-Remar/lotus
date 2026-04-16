#include "Concurrency/CUDA/CUDAAnalysis.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallPtrSet.h>
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

static bool isNVVMKernel(const Function *function) {
  return function && (function->hasFnAttribute("nvvm.kernel") ||
                      function->getCallingConv() == CallingConv::PTX_Kernel);
}

static bool isSharedRaceRelevant(const AccessInfo &access) {
  return access.space == MemorySpace::Shared && access.base && !access.is_atomic;
}

static bool isGlobalRaceRelevant(const AccessInfo &access) {
  return (access.space == MemorySpace::Global ||
          access.space == MemorySpace::Device) &&
         access.base;
}

static SynchronizationScope getSyncScope(ThreadAPI::TD_TYPE type) {
  switch (type) {
  case ThreadAPI::TD_CUDA_WARP_BARRIER:
    return SynchronizationScope::Warp;
  case ThreadAPI::TD_CUDA_BARRIER:
    return SynchronizationScope::Block;
  case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
  case ThreadAPI::TD_CUDA_DEVICE_SYNC:
    return SynchronizationScope::Device;
  default:
    return SynchronizationScope::None;
  }
}

static bool sharesBaseObject(const AccessInfo &lhs, const AccessInfo &rhs) {
  if (!lhs.base || !rhs.base) {
    return false;
  }
  if (lhs.base == rhs.base) {
    return true;
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

static bool accessesConflict(const AccessInfo &lhs, const AccessInfo &rhs) {
  return sharesBaseObject(lhs, rhs) && (lhs.is_write || rhs.is_write);
}

static SmallVector<const Value *, 4>
mergeBaseObjects(const AccessInfo &lhs, const AccessInfo &rhs) {
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

static bool isDeviceSyncInstruction(const Instruction *inst, ThreadAPI *api) {
  const auto *call = dyn_cast_or_null<CallBase>(inst);
  return call && api->getType(call) == ThreadAPI::TD_CUDA_DEVICE_SYNC;
}

static bool isOrderedBetweenLaunches(const Instruction *first,
                                     const Instruction *second,
                                     ThreadAPI *api) {
  if (!first || !second || first->getFunction() != second->getFunction()) {
    return false;
  }

  bool seen_first = false;
  for (const Instruction &inst : instructions(*first->getFunction())) {
    if (&inst == first) {
      seen_first = true;
      continue;
    }
    if (&inst == second) {
      return false;
    }
    if (seen_first && isDeviceSyncInstruction(&inst, api)) {
      return true;
    }
  }
  return false;
}

static bool mustReachBarrier(const Instruction *barrier, const Function *kernel) {
  const BasicBlock *barrier_block = barrier ? barrier->getParent() : nullptr;
  const BasicBlock *entry = kernel && !kernel->empty() ? &kernel->getEntryBlock()
                                                       : nullptr;
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
  uint64_t threads = static_cast<uint64_t>(getConcreteExtent(dims.block[0], 1)) *
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

static bool isBlockUniformBuiltin(BuiltinKind kind) {
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

static std::optional<int64_t>
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
          const auto lhs_addr =
              evaluateAddressForThread(lhs.address_pattern, dims, lane_l.tid_x,
                                       lane_l.tid_y, lane_l.tid_z, block_l, 0,
                                       0);
          const auto rhs_addr =
              evaluateAddressForThread(rhs.address_pattern, dims, lane_r.tid_x,
                                       lane_r.tid_y, lane_r.tid_z, block_r, 0,
                                       0);
          if (lhs_addr && rhs_addr && lhs_addr == rhs_addr) {
            return true;
          }
        }
      }
    }
  }

  return false;
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
    auto addr = evaluateAddressForThread(access.address_pattern, dims,
                                         coords.tid_x, coords.tid_y,
                                         coords.tid_z, 0, 0, 0);
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
    auto addr = evaluateAddressForThread(access.address_pattern, dims,
                                         coords.tid_x, coords.tid_y,
                                         coords.tid_z, 0, 0, 0);
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

bool LaunchDimensions::hasSymbolicGrid() const {
  return llvm::any_of(grid, [](const SymbolicDimension &dim) {
    return dim.kind != SymbolicValueKind::Constant;
  });
}

bool LaunchDimensions::hasSymbolicBlock() const {
  return llvm::any_of(block, [](const SymbolicDimension &dim) {
    return dim.kind != SymbolicValueKind::Constant;
  });
}

CUDAAnalysis::CUDAAnalysis(Module &module, DeviceConfig config)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()),
      m_device_config(config) {}

void CUDAAnalysis::runAnalysis() {
  m_launches.clear();
  m_kernel_summaries.clear();
  m_kernel_index.clear();
  m_inter_kernel_races.clear();
  size_t launch_sequence = 0;

  for (Function &function : m_module) {
    if (function.isDeclaration()) {
      continue;
    }

    SmallVector<const CallBase *, 8> block_calls;
    for (const BasicBlock &bb : function) {
      block_calls.clear();
      for (const Instruction &inst : bb) {
        if (const auto *call = dyn_cast<CallBase>(&inst)) {
          block_calls.push_back(call);
        }
      }

      for (size_t i = 0; i < block_calls.size(); ++i) {
        const CallBase *call = block_calls[i];
        const Function *callee = m_thread_api->getCallee(call);
        if (!callee ||
            m_thread_api->getType(callee) != ThreadAPI::TD_CUDA_KERNEL_LAUNCH) {
          continue;
        }

        KernelLaunchInfo launch;
        launch.launch = call;
        launch.dimensions = getLaunchDimensions(call);
        launch.sequence = launch_sequence++;
        if (i + 1 < block_calls.size()) {
          launch.kernel = m_thread_api->getCallee(block_calls[i + 1]);
        }
        if (!m_launches.empty()) {
          launch.ordered_after_previous =
              isOrderedBetweenLaunches(m_launches.back().launch, launch.launch,
                                       m_thread_api);
          launch.ordering_scope = launch.ordered_after_previous
                                      ? SynchronizationScope::Device
                                      : SynchronizationScope::None;
        }
        m_launches.push_back(launch);

        if (launch.kernel && !m_kernel_index.count(launch.kernel)) {
          analyzeKernel(launch.kernel, &m_launches.back());
        }
      }
    }
  }

  for (Function &function : m_module) {
    if (function.isDeclaration() || !isNVVMKernel(&function) ||
        m_kernel_index.count(&function)) {
      continue;
    }
    analyzeKernel(&function, nullptr);
  }

  analyzeInterKernelRaces();
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
  analyzeBarrierPhases(summary, kernel);
  analyzeRaces(summary);
  analyzeVolatile(summary);

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
  access.exact_address = access.address_pattern.valid && access.address_pattern.exact;
  if (const auto *call = dyn_cast<CallBase>(inst)) {
    access.ordering_scope = getSyncScope(m_thread_api->getType(call));
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

static bool isOrderedByBarrier(const AccessInfo &lhs, const AccessInfo &rhs,
                               const KernelSummary &summary) {
  for (const auto &phase : summary.barrier_phases) {
    const Instruction *barrier_inst = phase.barrier;
    if (!barrier_inst)
      continue;

    bool lhs_before = false;
    bool rhs_after = false;
    if (lhs.inst && lhs.inst->getParent() == barrier_inst->getParent()) {
      lhs_before = comesBeforeInBlock(lhs.inst, barrier_inst);
    }
    if (!lhs_before) {
      for (const auto *pred : phase.preceding_blocks) {
        if (lhs.inst && lhs.inst->getParent() == pred) {
          lhs_before = true;
          break;
        }
      }
    }
    if (rhs.inst && rhs.inst->getParent() == barrier_inst->getParent()) {
      rhs_after = comesBeforeInBlock(barrier_inst, rhs.inst);
    }
    if (!rhs_after) {
      for (const auto *succ : phase.following_blocks) {
        if (rhs.inst && rhs.inst->getParent() == succ) {
          rhs_after = true;
          break;
        }
      }
    }
    if (phase.all_threads_reach && lhs_before && rhs_after &&
        (phase.scope == SynchronizationScope::Block ||
         phase.scope == SynchronizationScope::Device)) {
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
      if (!accessesConflict(lhs, rhs)) {
        continue;
      }

      if (lhs.is_atomic || rhs.is_atomic) {
        continue;
      }

      if (isOrderedByBarrier(lhs, rhs, summary)) {
        continue;
      }

      if (lhs.space == MemorySpace::Shared &&
          isUniformControlled(lhs, rhs, summary)) {
        continue;
      }

      if (isSharedRaceRelevant(lhs) && isSharedRaceRelevant(rhs)) {
        if (hasDistinctThreadAlias(lhs, rhs, summary.dimensions,
                                   m_device_config, false)) {
          summary.has_shared_race = true;
          RaceInfo race;
          race.first = lhs.inst;
          race.second = rhs.inst;
          race.base = lhs.base;
          race.bases = mergeBaseObjects(lhs, rhs);
          race.space = MemorySpace::Shared;
          race.same_block_only = true;
          race.cross_block = false;
          race.symbolic = summary.dimensions.hasSymbolicBlock() ||
                          !lhs.exact_address || !rhs.exact_address ||
                          lhs.has_ambiguous_base || rhs.has_ambiguous_base;
          race.kind = RaceKind::DataRace;
          race.scope = SynchronizationScope::Block;
          race.ordering_reason = "missing block synchronization";
          race.exact = !race.symbolic;
          summary.shared_races.push_back(std::move(race));
        }
      } else if (isGlobalRaceRelevant(lhs) && isGlobalRaceRelevant(rhs)) {
        const bool cross_block =
            lhs.depends_on_block_idx || rhs.depends_on_block_idx;
        const bool aliases = hasDistinctThreadAlias(lhs, rhs, summary.dimensions,
                                                    m_device_config, cross_block);
        if (aliases) {
          summary.has_global_race = true;
          RaceInfo race;
          race.first = lhs.inst;
          race.second = rhs.inst;
          race.base = lhs.base;
          race.bases = mergeBaseObjects(lhs, rhs);
          race.space = lhs.space;
          race.same_block_only = !cross_block;
          race.cross_block = cross_block;
          race.symbolic = summary.dimensions.hasSymbolicGrid() ||
                          summary.dimensions.hasSymbolicBlock() ||
                          !lhs.exact_address || !rhs.exact_address ||
                          lhs.has_ambiguous_base || rhs.has_ambiguous_base;
          race.kind = (lhs.is_atomic || rhs.is_atomic) ? RaceKind::AtomicOrderingRisk
                                                       : RaceKind::DataRace;
          race.scope = cross_block ? SynchronizationScope::Device
                                   : SynchronizationScope::Block;
          race.ordering_reason = cross_block ? "missing device ordering"
                                             : "missing intra-kernel ordering";
          race.exact = !race.symbolic;
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

    const UniformityClass uniformity = classifyUniformity(branch->getCondition());
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

void CUDAAnalysis::analyzeBarrierPhases(KernelSummary &summary,
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
          type != ThreadAPI::TD_CUDA_WARP_BARRIER) {
        continue;
      }

      BarrierPhaseInfo info;
      info.barrier = &inst;
      info.scope = getSyncScope(type);

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
      info.all_threads_reach = mustReachBarrier(&inst, kernel);
      info.exact = info.all_threads_reach;
      summary.barrier_phases.push_back(std::move(info));
    }
  }
}

void CUDAAnalysis::analyzeInterKernelRaces() {
  if (m_launches.size() < 2) {
    return;
  }

  for (size_t i = 0; i < m_launches.size(); ++i) {
    for (size_t j = i + 1; j < m_launches.size(); ++j) {
      const KernelLaunchInfo &launch_a = m_launches[i];
      const KernelLaunchInfo &launch_b = m_launches[j];

      if (!launch_a.kernel || !launch_b.kernel) {
        continue;
      }

      size_t idx_a = m_kernel_index[launch_a.kernel];
      size_t idx_b = m_kernel_index[launch_b.kernel];
      const KernelSummary &summary_a = m_kernel_summaries[idx_a];
      const KernelSummary &summary_b = m_kernel_summaries[idx_b];

      for (const AccessInfo &access_a : summary_a.accesses) {
        if (!access_a.base)
          continue;
        if (access_a.space != MemorySpace::Global &&
            access_a.space != MemorySpace::Device) {
          continue;
        }

        for (const AccessInfo &access_b : summary_b.accesses) {
          if (!access_b.base)
            continue;
          if (access_b.space != access_a.space)
            continue;
          if (access_b.base != access_a.base)
            continue;

          if (!sharesBaseObject(access_a, access_b)) {
            continue;
          }
          if ((access_a.is_write || access_b.is_write) &&
              access_a.is_atomic && access_b.is_atomic) {
            continue;
          }

          InterKernelRaceInfo race;
          race.first_launch = launch_a.launch;
          race.second_launch = launch_b.launch;
          race.first_kernel = launch_a.kernel;
          race.second_kernel = launch_b.kernel;
          race.shared_base = access_a.base;
          race.ordered = launch_b.ordered_after_previous;
          race.ordering_reason = race.ordered ? "device_sync" : "unordered";
          race.symbolic = launch_a.dimensions.hasSymbolicGrid() ||
                          launch_a.dimensions.hasSymbolicBlock() ||
                          launch_b.dimensions.hasSymbolicGrid() ||
                          launch_b.dimensions.hasSymbolicBlock();
          race.kind = (access_a.is_atomic || access_b.is_atomic)
                          ? RaceKind::AtomicOrderingRisk
                          : RaceKind::InterKernelHazard;
          if (race.ordered) {
            continue;
          }
          m_inter_kernel_races.push_back(std::move(race));
        }
      }
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

LaunchDimensions CUDAAnalysis::getLaunchDimensions(const Instruction *launch) {
  LaunchDimensions dims;
  const auto *call = dyn_cast_or_null<CallBase>(launch);
  if (!call) {
    return dims;
  }

  for (unsigned i = 0; i < 3; ++i) {
    dims.grid[i].kind = SymbolicValueKind::Constant;
    dims.grid[i].constant = 1;
    dims.block[i].kind = SymbolicValueKind::Constant;
    dims.block[i].constant = 1;
  }

  if (call->arg_size() > 0) {
    dims.grid[0] = classifyDimension(call->getArgOperand(0));
  }
  if (call->arg_size() > 1) {
    dims.block[0] = classifyDimension(call->getArgOperand(1));
  }
  if (call->arg_size() > 2) {
    dims.grid[1] = classifyDimension(call->getArgOperand(2));
  }
  if (call->arg_size() > 3) {
    dims.block[1] = classifyDimension(call->getArgOperand(3));
  }
  if (call->arg_size() > 4) {
    dims.grid[2] = classifyDimension(call->getArgOperand(4));
  }
  if (call->arg_size() > 5) {
    dims.block[2] = classifyDimension(call->getArgOperand(5));
  }

  return dims;
}

} // namespace concurrency::cuda
