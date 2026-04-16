#include "Concurrency/CUDA/CUDAAnalysis.h"

#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>

using namespace llvm;

namespace concurrency::cuda {

namespace {

static bool isNVVMKernel(const Function *function) {
  return function && (function->hasFnAttribute("nvvm.kernel") ||
                      function->getCallingConv() == CallingConv::PTX_Kernel);
}

static bool isCUDAThreadBuiltinName(StringRef name) {
  return name.equals("threadIdx") || name.equals("threadIdx.x") ||
         name.equals("threadIdx.y") || name.equals("threadIdx.z") ||
         name.equals("blockIdx") || name.equals("blockIdx.x") ||
         name.equals("blockIdx.y") || name.equals("blockIdx.z") ||
         name.equals("blockDim") || name.equals("blockDim.x") ||
         name.equals("blockDim.y") || name.equals("blockDim.z") ||
         name.equals("gridDim") || name.equals("gridDim.x") ||
         name.equals("gridDim.y") || name.equals("gridDim.z") ||
         name.contains("tid.") || name.contains("ctaid.") ||
         name.contains("ntid.") || name.contains("nctaid.") ||
         name.contains("laneid");
}

static const Value *stripCastsAndGEPBase(const Value *value) {
  const Value *current = value ? value->stripPointerCasts() : nullptr;
  while (current) {
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      current = gep->getPointerOperand()->stripPointerCasts();
      continue;
    }
    if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      if (ce->isCast()) {
        current = ce->getOperand(0)->stripPointerCasts();
        continue;
      }
      if (ce->getOpcode() == Instruction::GetElementPtr) {
        current = ce->getOperand(0)->stripPointerCasts();
        continue;
      }
    }
    break;
  }
  return current;
}

static bool valueContainsName(const Value *value, StringRef needle) {
  if (!value) {
    return false;
  }
  if (const auto *gv = dyn_cast<GlobalValue>(value)) {
    return gv->getName().contains(needle);
  }
  if (const auto *inst = dyn_cast<Instruction>(value)) {
    return inst->hasName() && inst->getName().contains(needle);
  }
  if (const auto *arg = dyn_cast<Argument>(value)) {
    return arg->hasName() && arg->getName().contains(needle);
  }
  return false;
}

static std::optional<int64_t> addIfKnown(const std::optional<int64_t> &lhs,
                                         const std::optional<int64_t> &rhs,
                                         bool subtract = false) {
  if (!lhs || !rhs) {
    return std::nullopt;
  }
  return subtract ? (*lhs - *rhs) : (*lhs + *rhs);
}

static bool isSharedRaceRelevant(const AccessInfo &access) {
  return access.space == MemorySpace::Shared && access.base && !access.is_atomic;
}

static bool isGlobalRaceRelevant(const AccessInfo &access) {
  return (access.space == MemorySpace::Global || access.space == MemorySpace::Device) &&
         access.base && !access.is_atomic;
}

static bool accessesConflict(const AccessInfo &lhs, const AccessInfo &rhs) {
  return lhs.base && lhs.base == rhs.base && (lhs.is_write || rhs.is_write);
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

static uint32_t getActiveLaneCount(const LaunchDimensions &dims,
                                   const DeviceConfig &config) {
  return std::min(config.warp_size,
                  getConcreteExtent(dims.block[0], config.warp_size));
}

static std::optional<int64_t>
evaluateAddressForThread(const AffineAccessPattern &pattern, int64_t tid_x,
                         int64_t block_x) {
  if (!pattern.valid) {
    return std::nullopt;
  }
  return pattern.constant + pattern.thread_idx_x * tid_x +
         pattern.block_idx_x * block_x;
}

static bool hasDistinctThreadAlias(const AccessInfo &lhs, const AccessInfo &rhs,
                                   const LaunchDimensions &dims,
                                   const DeviceConfig &config,
                                   bool allow_cross_block) {
  if (!lhs.address_pattern.valid || !rhs.address_pattern.valid) {
    return lhs.depends_on_thread_idx || rhs.depends_on_thread_idx ||
           lhs.depends_on_block_idx || rhs.depends_on_block_idx;
  }

  const uint32_t lanes = getActiveLaneCount(dims, config);
  const uint32_t block_count =
      allow_cross_block ? std::min<uint32_t>(2, getConcreteExtent(dims.grid[0], 2))
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
          const auto lhs_addr =
              evaluateAddressForThread(lhs.address_pattern, tid_l, block_l);
          const auto rhs_addr =
              evaluateAddressForThread(rhs.address_pattern, tid_r, block_r);
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

  for (uint32_t lane = 0; lane < lanes; ++lane) {
    auto addr = evaluateAddressForThread(access.address_pattern, lane, 0);
    if (!addr) {
      return std::nullopt;
    }

    SmallVector<bool, 32> touched_banks(bank_count, false);
    for (uint32_t byte = 0; byte < access.access_size; ++byte) {
      const int64_t absolute = *addr + byte;
      const uint32_t bank =
          static_cast<uint32_t>((absolute / bank_width) % bank_count);
      touched_banks[bank] = true;
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
  if (max_occupancy <= 1) {
    return std::nullopt;
  }

  BankConflictInfo info;
  info.inst = access.inst;
  info.bank_count = bank_count;
  info.bank_width = bank_width;
  info.conflict_degree = max_occupancy;
  info.threads_per_bank = max_occupancy;
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
  for (uint32_t lane = 0; lane < lanes; ++lane) {
    auto addr = evaluateAddressForThread(access.address_pattern, lane, 0);
    if (!addr) {
      return std::nullopt;
    }
    const uint32_t start =
        static_cast<uint32_t>(*addr / transaction_bytes);
    const uint32_t end =
        static_cast<uint32_t>((*addr + access.access_size - 1) / transaction_bytes);
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
  if (segments.size() <= 1) {
    info.quality = CoalescingQuality::FullyCoalesced;
  } else if (segments.size() <= 4) {
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

  for (Function &function : m_module) {
    if (function.isDeclaration()) {
      continue;
    }

    for (inst_iterator it = inst_begin(function), end = inst_end(function);
         it != end; ++it) {
      const Instruction *inst = &*it;
      const auto *call = dyn_cast<CallBase>(inst);
      if (!call) {
        continue;
      }

      if (!m_thread_api->isTDFork(call) ||
          m_thread_api->getType(m_thread_api->getCallee(call)) !=
              ThreadAPI::TD_CUDA_KERNEL_LAUNCH) {
        continue;
      }

      KernelLaunchInfo launch;
      launch.launch = inst;
      launch.kernel = dyn_cast_or_null<Function>(m_thread_api->getForkedFun(inst));
      launch.dimensions = getLaunchDimensions(inst);
      m_launches.push_back(launch);

      if (launch.kernel && !m_kernel_index.count(launch.kernel)) {
        analyzeKernel(launch.kernel, &m_launches.back());
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
}

void CUDAAnalysis::analyzeKernel(const Function *kernel,
                                 const KernelLaunchInfo *launch) {
  KernelSummary summary;
  summary.kernel = kernel;
  if (launch) {
    summary.dimensions = launch->dimensions;
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
  access.base = getCanonicalBase(pointer);
  access.space = classifyMemorySpace(pointer);
  access.is_write = is_write;
  access.is_atomic =
      isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
  if (const auto *load = dyn_cast<LoadInst>(inst)) {
    access.is_volatile = load->isVolatile();
  } else if (const auto *store = dyn_cast<StoreInst>(inst)) {
    access.is_volatile = store->isVolatile();
  }
  access.depends_on_thread_idx = dependsOnThreadBuiltin(pointer);
  access.depends_on_block_idx = dependsOnBlockBuiltin(pointer);
  access.depends_on_lane_id = dependsOnLaneBuiltin(pointer);
  access.address_pattern = extractAffineAccessPattern(pointer);

  const DataLayout &dl = m_module.getDataLayout();
  if (const auto *load = dyn_cast<LoadInst>(inst)) {
    access.access_size =
        static_cast<uint32_t>(dl.getTypeStoreSize(load->getType()));
  } else if (const auto *store = dyn_cast<StoreInst>(inst)) {
    access.access_size =
        static_cast<uint32_t>(dl.getTypeStoreSize(store->getValueOperand()->getType()));
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
      if (auto conflict =
              computeBankConflict(access, summary.dimensions, m_device_config)) {
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
          info &&
          (info->quality == CoalescingQuality::PartiallyCoalesced ||
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

    DivergenceRegion region;
    region.branch = branch;
    region.depends_on_thread_idx = dependsOnThreadBuiltin(branch->getCondition());
    region.depends_on_lane_id = dependsOnLaneBuiltin(branch->getCondition());

    if (const DomTreeNodeBase<BasicBlock> *node = pdt.getNode(&bb)) {
      if (const DomTreeNodeBase<BasicBlock> *idom = node->getIDom()) {
        region.merge_block = idom->getBlock();
      }
    }
    if (!region.merge_block) {
      region.merge_block = pdt.findNearestCommonDominator(branch->getSuccessor(0),
                                                          branch->getSuccessor(1));
    }

    for (unsigned succ_index = 0; succ_index < branch->getNumSuccessors();
         ++succ_index) {
      for (const BasicBlock *reachable :
           collectReachableBlocks(branch->getSuccessor(succ_index),
                                  region.merge_block)) {
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

void CUDAAnalysis::analyzeRaces(KernelSummary &summary) {
  for (size_t i = 0; i < summary.accesses.size(); ++i) {
    for (size_t j = i + 1; j < summary.accesses.size(); ++j) {
      const AccessInfo &lhs = summary.accesses[i];
      const AccessInfo &rhs = summary.accesses[j];
      if (!accessesConflict(lhs, rhs)) {
        continue;
      }
      if (isSharedRaceRelevant(lhs) && isSharedRaceRelevant(rhs)) {
        if (hasDistinctThreadAlias(lhs, rhs, summary.dimensions, m_device_config,
                                   false)) {
          summary.has_shared_race = true;
          summary.shared_races.push_back(
              {lhs.inst, rhs.inst, lhs.base, MemorySpace::Shared, true, false});
        }
      } else if (isGlobalRaceRelevant(lhs) && isGlobalRaceRelevant(rhs)) {
        const bool cross_block =
            lhs.depends_on_block_idx || rhs.depends_on_block_idx;
        if (hasDistinctThreadAlias(lhs, rhs, summary.dimensions, m_device_config,
                                   cross_block)) {
          summary.has_global_race = true;
          summary.global_races.push_back(
              {lhs.inst, rhs.inst, lhs.base, lhs.space, !cross_block, cross_block});
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
    if (access.space != MemorySpace::Shared && access.space != MemorySpace::Global &&
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

MemorySpace CUDAAnalysis::classifyMemorySpace(const Value *value) {
  const Value *base = stripCastsAndGEPBase(value);
  if (!base) {
    return MemorySpace::Unknown;
  }

  if (isa<AllocaInst>(base)) {
    return MemorySpace::Local;
  }

  auto classify_addrspace = [](unsigned addrspace) {
    switch (addrspace) {
    case 0:
      return MemorySpace::Host;
    case 1:
      return MemorySpace::Global;
    case 3:
      return MemorySpace::Shared;
    case 4:
      return MemorySpace::Constant;
    case 5:
      return MemorySpace::Local;
    case 101:
      return MemorySpace::Device;
    default:
      return MemorySpace::Unknown;
    }
  };

  if (const auto *ptr_ty = dyn_cast<PointerType>(base->getType())) {
    MemorySpace by_as = classify_addrspace(ptr_ty->getAddressSpace());
    if (by_as != MemorySpace::Unknown) {
      return by_as;
    }
  }

  if (const auto *gv = dyn_cast<GlobalValue>(base)) {
    if (gv->hasSection()) {
      StringRef section = gv->getSection();
      if (section.contains("shared")) {
        return MemorySpace::Shared;
      }
      if (section.contains("constant")) {
        return MemorySpace::Constant;
      }
      if (section.contains("device")) {
        return MemorySpace::Device;
      }
    }
    if (gv->getName().startswith("__device_")) {
      return MemorySpace::Device;
    }
    if (isCUDAThreadBuiltinName(gv->getName())) {
      return MemorySpace::Local;
    }
    return MemorySpace::Host;
  }
  if (const auto *arg = dyn_cast<Argument>(base)) {
    if (base->getType()->getPointerAddressSpace() == 1) {
      return MemorySpace::Global;
    }
    if (arg->hasByValAttr()) {
      return MemorySpace::Host;
    }
    if (arg->hasName()) {
      if (arg->getName().contains("shared")) {
        return MemorySpace::Shared;
      }
      if (arg->getName().contains("constant")) {
        return MemorySpace::Constant;
      }
      if (arg->getName().contains("device")) {
        return MemorySpace::Device;
      }
    }
    if (arg->getParent() && isNVVMKernel(arg->getParent())) {
      return MemorySpace::Global;
    }
    return MemorySpace::Unknown;
  }

  return MemorySpace::Unknown;
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

const Value *CUDAAnalysis::getCanonicalBase(const Value *value) {
  return stripCastsAndGEPBase(value);
}

BuiltinKind CUDAAnalysis::classifyBuiltin(const Value *value) {
  if (!value) {
    return BuiltinKind::None;
  }

  auto classify_name = [](StringRef name) {
    if (name.contains("tid.x") || name.contains("threadIdx.x")) {
      return BuiltinKind::ThreadIdxX;
    }
    if (name.contains("tid.y") || name.contains("threadIdx.y")) {
      return BuiltinKind::ThreadIdxY;
    }
    if (name.contains("tid.z") || name.contains("threadIdx.z")) {
      return BuiltinKind::ThreadIdxZ;
    }
    if (name.contains("ctaid.x") || name.contains("blockIdx.x")) {
      return BuiltinKind::BlockIdxX;
    }
    if (name.contains("ctaid.y") || name.contains("blockIdx.y")) {
      return BuiltinKind::BlockIdxY;
    }
    if (name.contains("ctaid.z") || name.contains("blockIdx.z")) {
      return BuiltinKind::BlockIdxZ;
    }
    if (name.contains("ntid.x") || name.contains("blockDim.x")) {
      return BuiltinKind::BlockDimX;
    }
    if (name.contains("ntid.y") || name.contains("blockDim.y")) {
      return BuiltinKind::BlockDimY;
    }
    if (name.contains("ntid.z") || name.contains("blockDim.z")) {
      return BuiltinKind::BlockDimZ;
    }
    if (name.contains("nctaid.x") || name.contains("gridDim.x")) {
      return BuiltinKind::GridDimX;
    }
    if (name.contains("nctaid.y") || name.contains("gridDim.y")) {
      return BuiltinKind::GridDimY;
    }
    if (name.contains("nctaid.z") || name.contains("gridDim.z")) {
      return BuiltinKind::GridDimZ;
    }
    if (name.contains("laneid")) {
      return BuiltinKind::LaneId;
    }
    return BuiltinKind::None;
  };

  if (const auto *call = dyn_cast<CallBase>(value)) {
    if (const Function *callee = call->getCalledFunction()) {
      return classify_name(callee->getName());
    }
  }
  if (const auto *gv = dyn_cast<GlobalValue>(value)) {
    return classify_name(gv->getName());
  }
  if (const auto *inst = dyn_cast<Instruction>(value)) {
    if (inst->hasName()) {
      BuiltinKind kind = classify_name(inst->getName());
      if (kind != BuiltinKind::None) {
        return kind;
      }
    }
  }
  if (const auto *arg = dyn_cast<Argument>(value)) {
    if (arg->hasName()) {
      return classify_name(arg->getName());
    }
  }
  return BuiltinKind::None;
}

bool CUDAAnalysis::dependsOnThreadBuiltin(const Value *value) {
  if (!value) {
    return false;
  }
  SmallVector<const Value *, 8> worklist;
  SmallPtrSet<const Value *, 16> visited;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.pop_back_val();
    if (!visited.insert(current).second) {
      continue;
    }

    BuiltinKind builtin = classifyBuiltin(current);
    if (builtin == BuiltinKind::ThreadIdxX || builtin == BuiltinKind::ThreadIdxY ||
        builtin == BuiltinKind::ThreadIdxZ || builtin == BuiltinKind::LaneId) {
      return true;
    }

    if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Value *operand : inst->operands()) {
        worklist.push_back(operand);
      }
    } else if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      for (const Value *operand : ce->operands()) {
        worklist.push_back(operand);
      }
    }
  }

  return false;
}

bool CUDAAnalysis::dependsOnBlockBuiltin(const Value *value) {
  if (!value) {
    return false;
  }
  SmallVector<const Value *, 8> worklist;
  SmallPtrSet<const Value *, 16> visited;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.pop_back_val();
    if (!visited.insert(current).second) {
      continue;
    }

    BuiltinKind builtin = classifyBuiltin(current);
    if (builtin == BuiltinKind::BlockIdxX || builtin == BuiltinKind::BlockIdxY ||
        builtin == BuiltinKind::BlockIdxZ) {
      return true;
    }

    if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Value *operand : inst->operands()) {
        worklist.push_back(operand);
      }
    } else if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      for (const Value *operand : ce->operands()) {
        worklist.push_back(operand);
      }
    }
  }

  return false;
}

bool CUDAAnalysis::dependsOnLaneBuiltin(const Value *value) {
  if (!value) {
    return false;
  }
  SmallVector<const Value *, 8> worklist;
  SmallPtrSet<const Value *, 16> visited;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.pop_back_val();
    if (!visited.insert(current).second) {
      continue;
    }
    if (classifyBuiltin(current) == BuiltinKind::LaneId) {
      return true;
    }
    if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Value *operand : inst->operands()) {
        worklist.push_back(operand);
      }
    } else if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      for (const Value *operand : ce->operands()) {
        worklist.push_back(operand);
      }
    }
  }

  return false;
}

std::optional<int64_t> CUDAAnalysis::evaluateConstantInt(const Value *value) {
  if (!value) {
    return std::nullopt;
  }
  if (const auto *ci = dyn_cast<ConstantInt>(value)) {
    return ci->getSExtValue();
  }
  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->getOpcode() == Instruction::Add) {
      return addIfKnown(evaluateConstantInt(ce->getOperand(0)),
                        evaluateConstantInt(ce->getOperand(1)));
    }
    if (ce->getOpcode() == Instruction::Sub) {
      return addIfKnown(evaluateConstantInt(ce->getOperand(0)),
                        evaluateConstantInt(ce->getOperand(1)), true);
    }
    if (ce->getOpcode() == Instruction::Mul) {
      auto lhs = evaluateConstantInt(ce->getOperand(0));
      auto rhs = evaluateConstantInt(ce->getOperand(1));
      if (lhs && rhs) {
        return (*lhs) * (*rhs);
      }
    }
    return std::nullopt;
  }
  if (const auto *inst = dyn_cast<Instruction>(value)) {
    if (inst->getOpcode() == Instruction::Add) {
      return addIfKnown(evaluateConstantInt(inst->getOperand(0)),
                        evaluateConstantInt(inst->getOperand(1)));
    }
    if (inst->getOpcode() == Instruction::Sub) {
      return addIfKnown(evaluateConstantInt(inst->getOperand(0)),
                        evaluateConstantInt(inst->getOperand(1)), true);
    }
    if (inst->getOpcode() == Instruction::Mul ||
        inst->getOpcode() == Instruction::Shl) {
      auto lhs = evaluateConstantInt(inst->getOperand(0));
      auto rhs = evaluateConstantInt(inst->getOperand(1));
      if (lhs && rhs) {
        return inst->getOpcode() == Instruction::Mul ? (*lhs) * (*rhs)
                                                     : (*lhs) << (*rhs);
      }
    }
  }
  return std::nullopt;
}

AffineAccessPattern CUDAAnalysis::extractAffineAccessPattern(const Value *value) {
  AffineAccessPattern pattern;
  if (!value) {
    return pattern;
  }

  BuiltinKind builtin = classifyBuiltin(value);
  switch (builtin) {
  case BuiltinKind::ThreadIdxX:
    pattern.thread_idx_x = 1;
    pattern.valid = true;
    return pattern;
  case BuiltinKind::BlockIdxX:
    pattern.block_idx_x = 1;
    pattern.valid = true;
    return pattern;
  case BuiltinKind::LaneId:
    pattern.lane_id = 1;
    pattern.valid = true;
    return pattern;
  default:
    break;
  }

  if (const auto *ci = dyn_cast<ConstantInt>(value)) {
    pattern.constant = ci->getSExtValue();
    pattern.valid = true;
    return pattern;
  }

  auto merge = [](AffineAccessPattern &dst, const AffineAccessPattern &lhs,
                  const AffineAccessPattern &rhs, bool subtract = false) {
    if (!lhs.valid || !rhs.valid) {
      dst.valid = false;
      return;
    }
    dst.constant = subtract ? lhs.constant - rhs.constant
                            : lhs.constant + rhs.constant;
    dst.thread_idx_x = subtract ? lhs.thread_idx_x - rhs.thread_idx_x
                                : lhs.thread_idx_x + rhs.thread_idx_x;
    dst.block_idx_x = subtract ? lhs.block_idx_x - rhs.block_idx_x
                               : lhs.block_idx_x + rhs.block_idx_x;
    dst.lane_id = subtract ? lhs.lane_id - rhs.lane_id
                           : lhs.lane_id + rhs.lane_id;
    dst.valid = true;
  };

  if (const auto *gep = dyn_cast<GEPOperator>(value)) {
    if (gep->getNumIndices() == 0) {
      return extractAffineAccessPattern(gep->getPointerOperand());
    }
    const Value *last_index = gep->idx_end()[-1];
    AffineAccessPattern index_pattern = extractAffineAccessPattern(last_index);
    if (!index_pattern.valid) {
      return pattern;
    }
    Type *element_type = gep->getSourceElementType();
    int64_t elem_size = 4;
    if (element_type->isIntegerTy()) {
      elem_size = std::max<int64_t>(1, element_type->getIntegerBitWidth() / 8);
    } else if (element_type->isFloatTy()) {
      elem_size = 4;
    } else if (element_type->isDoubleTy()) {
      elem_size = 8;
    } else if (element_type->isPointerTy()) {
      elem_size = 8;
    }
    index_pattern.constant *= elem_size;
    index_pattern.thread_idx_x *= elem_size;
    index_pattern.block_idx_x *= elem_size;
    index_pattern.lane_id *= elem_size;
    return index_pattern;
  }

  if (const auto *op = dyn_cast<Operator>(value)) {
    if (op->getOpcode() == Instruction::Add) {
      AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(0));
      AffineAccessPattern rhs = extractAffineAccessPattern(op->getOperand(1));
      merge(pattern, lhs, rhs);
      return pattern;
    }
    if (op->getOpcode() == Instruction::Sub) {
      AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(0));
      AffineAccessPattern rhs = extractAffineAccessPattern(op->getOperand(1));
      merge(pattern, lhs, rhs, true);
      return pattern;
    }
    if (op->getOpcode() == Instruction::Mul ||
        op->getOpcode() == Instruction::Shl) {
      auto lhs_const = evaluateConstantInt(op->getOperand(0));
      auto rhs_const = evaluateConstantInt(op->getOperand(1));
      if (lhs_const && !rhs_const) {
        AffineAccessPattern rhs = extractAffineAccessPattern(op->getOperand(1));
        if (!rhs.valid) {
          return pattern;
        }
        const int64_t scale = *lhs_const;
        rhs.constant *= scale;
        rhs.thread_idx_x *= scale;
        rhs.block_idx_x *= scale;
        rhs.lane_id *= scale;
        return rhs;
      }
      if (!lhs_const && rhs_const) {
        AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(0));
        if (!lhs.valid) {
          return pattern;
        }
        const int64_t scale =
            op->getOpcode() == Instruction::Shl ? (int64_t{1} << *rhs_const)
                                                : *rhs_const;
        lhs.constant *= scale;
        lhs.thread_idx_x *= scale;
        lhs.block_idx_x *= scale;
        lhs.lane_id *= scale;
        return lhs;
      }
    }
  }

  return pattern;
}

SymbolicDimension CUDAAnalysis::classifyDimension(const Value *value) {
  SymbolicDimension dim;
  dim.value = value;
  if (!value) {
    return dim;
  }
  if (const auto constant = evaluateConstantInt(value)) {
    dim.kind = SymbolicValueKind::Constant;
    dim.constant = static_cast<uint64_t>(*constant);
    return dim;
  }
  if (classifyBuiltin(value) != BuiltinKind::None) {
    dim.kind = SymbolicValueKind::DerivedFromBuiltin;
    return dim;
  }
  if (isa<Argument>(value) || isa<Instruction>(value)) {
    dim.kind = SymbolicValueKind::Symbolic;
    return dim;
  }
  return dim;
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
