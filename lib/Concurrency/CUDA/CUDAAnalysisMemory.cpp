#include "Concurrency/CUDA/CUDAAnalysis.h"
#include "Concurrency/CUDA/CUDASemantics.h"

#include <limits>

#include <llvm/IR/InstIterator.h>

using namespace llvm;

namespace concurrency::cuda {

namespace {

static const Value *getLastArgument(const CallBase *call) {
  if (!call || call->arg_empty()) {
    return nullptr;
  }
  return call->getArgOperand(call->arg_size() - 1);
}

static const Value *getTransferStream(const CallBase *call) {
  if (!call) {
    return nullptr;
  }
  const Function *callee = call->getCalledFunction();
  if (!callee || !callee->getName().contains("Async")) {
    return nullptr;
  }
  return getLastArgument(call);
}

static bool isHostLikeSpace(MemorySpace space) {
  return space == MemorySpace::Host;
}

static bool isDeviceLikeSpace(MemorySpace space) {
  return space == MemorySpace::Device || space == MemorySpace::Global ||
         space == MemorySpace::Shared || space == MemorySpace::ClusterShared ||
         space == MemorySpace::Constant || space == MemorySpace::Local;
}

static TransferKind classifyTransferKind(MemorySpace src, MemorySpace dst) {
  if (isHostLikeSpace(src) && isDeviceLikeSpace(dst)) {
    return TransferKind::HostToDevice;
  }
  if (isDeviceLikeSpace(src) && isHostLikeSpace(dst)) {
    return TransferKind::DeviceToHost;
  }
  if (isDeviceLikeSpace(src) && isDeviceLikeSpace(dst)) {
    return TransferKind::DeviceToDevice;
  }
  if (isHostLikeSpace(src) && isHostLikeSpace(dst)) {
    return TransferKind::HostToHost;
  }
  return TransferKind::Unknown;
}

static std::optional<TransferKind> getExplicitMemcpyKind(
    const CallBase *call,
    unsigned explicit_index = std::numeric_limits<unsigned>::max()) {
  if (!call || !call->getCalledFunction()) {
    return std::nullopt;
  }
  const StringRef name = call->getCalledFunction()->getName();
  unsigned kind_index = explicit_index;
  if (kind_index == std::numeric_limits<unsigned>::max()) {
    kind_index = 3;
    if (name.contains("MemcpyToSymbol") || name.contains("MemcpyFromSymbol")) {
      kind_index = 4;
    } else if (name.contains("Memcpy2D") || name.contains("Memcpy3D") ||
               name.contains("MemcpyPeer")) {
      return std::nullopt;
    }
  }
  if (kind_index >= call->arg_size()) {
    return std::nullopt;
  }
  const auto *kind = dyn_cast<ConstantInt>(call->getArgOperand(kind_index));
  if (!kind) {
    return std::nullopt;
  }
  switch (kind->getZExtValue()) {
  case 0:
    return TransferKind::HostToHost;
  case 1:
    return TransferKind::HostToDevice;
  case 2:
    return TransferKind::DeviceToHost;
  case 3:
    return TransferKind::DeviceToDevice;
  default:
    return std::nullopt;
  }
}

struct DecodedMemcpy {
  const Value *dst = nullptr;
  const Value *src = nullptr;
  unsigned kind_index = std::numeric_limits<unsigned>::max();
  unsigned width_index = std::numeric_limits<unsigned>::max();
  unsigned height_index = std::numeric_limits<unsigned>::max();
  unsigned dst_pitch_index = std::numeric_limits<unsigned>::max();
  unsigned src_pitch_index = std::numeric_limits<unsigned>::max();
  bool region_unknown = false;
  bool execution_operation = true;
};

static DecodedMemcpy decodeMemcpy(const CallBase *call) {
  DecodedMemcpy decoded;
  const Function *callee = call ? call->getCalledFunction() : nullptr;
  const StringRef name = callee ? callee->getName() : StringRef{};
  if (!call) {
    return decoded;
  }
  if (name.contains("GraphAddMemcpy") || name.contains("MemcpyNode")) {
    decoded.execution_operation = false;
    return decoded;
  }
  if (name.contains("Memcpy3D") ||
      (name.startswith("cuMemcpy2D") && call->arg_size() <= 2)) {
    decoded.region_unknown = true;
    return decoded;
  }
  if (name.contains("Memcpy2DPeer")) {
    if (call->arg_size() >= 8) {
      decoded.dst = call->getArgOperand(0);
      decoded.src = call->getArgOperand(3);
      decoded.dst_pitch_index = 1;
      decoded.src_pitch_index = 4;
      decoded.width_index = 6;
      decoded.height_index = 7;
    }
    return decoded;
  }
  if (name.contains("Memcpy2D")) {
    if (call->arg_size() >= 6) {
      decoded.dst = call->getArgOperand(0);
      decoded.src = call->getArgOperand(2);
      decoded.dst_pitch_index = 1;
      decoded.src_pitch_index = 3;
      decoded.width_index = 4;
      decoded.height_index = 5;
      decoded.kind_index = 6;
    }
    return decoded;
  }
  if (name.contains("MemcpyPeer")) {
    if (call->arg_size() >= 5) {
      decoded.dst = call->getArgOperand(0);
      decoded.src = call->getArgOperand(2);
      decoded.width_index = 4;
    }
    return decoded;
  }
  if (name.contains("MemcpyToSymbol")) {
    if (call->arg_size() >= 3) {
      decoded.dst = call->getArgOperand(0);
      decoded.src = call->getArgOperand(1);
      decoded.width_index = 2;
      decoded.kind_index = 4;
    }
    return decoded;
  }
  if (name.contains("MemcpyFromSymbol")) {
    if (call->arg_size() >= 3) {
      decoded.dst = call->getArgOperand(0);
      decoded.src = call->getArgOperand(1);
      decoded.width_index = 2;
      decoded.kind_index = 4;
    }
    return decoded;
  }
  if (call->arg_size() >= 3) {
    decoded.dst = call->getArgOperand(0);
    decoded.src = call->getArgOperand(1);
    decoded.width_index = 2;
    if (name.startswith("cudaMemcpy")) {
      decoded.kind_index = 3;
    }
  }
  return decoded;
}

static uint64_t getConstantExtent(const CallBase *call, unsigned index,
                                  uint64_t fallback = 0) {
  if (!call || index >= call->arg_size()) {
    return fallback;
  }
  const auto value =
      CUDASymbolicModel::evaluateConstantInt(call->getArgOperand(index));
  return value && *value >= 0 ? static_cast<uint64_t>(*value) : fallback;
}

} // namespace

void CUDAAnalysis::analyzeMemoryTransfers() {
  m_memory_transfers.clear();

  for (Function &function : m_module) {
    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      ThreadAPI::TD_TYPE type = m_thread_api->getType(call);
      if (type != ThreadAPI::TD_CUDA_MEMCPY &&
          type != ThreadAPI::TD_CUDA_MEMSET &&
          type != ThreadAPI::TD_CUDA_MALLOC &&
          type != ThreadAPI::TD_CUDA_FREE &&
          type != ThreadAPI::TD_CUDA_UNIFIED_MEMORY) {
        continue;
      }

      if (type == ThreadAPI::TD_CUDA_MEMCPY && !call->arg_empty()) {
        const DecodedMemcpy decoded = decodeMemcpy(call);
        if (!decoded.execution_operation) {
          continue;
        }
        MemoryTransferInfo transfer;
        transfer.inst = &inst;
        transfer.host_function = &function;
        transfer.dst = decoded.dst;
        transfer.src = decoded.src;
        transfer.region_unknown = decoded.region_unknown;
        transfer.width = getConstantExtent(call, decoded.width_index, 0);
        transfer.height = getConstantExtent(call, decoded.height_index, 1);
        transfer.src_pitch =
            getConstantExtent(call, decoded.src_pitch_index, transfer.width);
        transfer.dst_pitch =
            getConstantExtent(call, decoded.dst_pitch_index, transfer.width);
        const uint64_t max_pitch =
            std::max(transfer.src_pitch, transfer.dst_pitch);
        if (transfer.height > 0 && max_pitch > 0 &&
            transfer.height - 1 <=
                (std::numeric_limits<uint64_t>::max() - transfer.width) /
                    max_pitch) {
          transfer.size = (transfer.height - 1) * max_pitch + transfer.width;
        } else {
          transfer.region_unknown = true;
        }
        transfer.is_async =
            call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains("Async");
        const Value *stream = getTransferStream(call);
        transfer.stream = stream;
        transfer.stream_known = stream != nullptr;

        const MemorySpaceInfo src_info =
            CUDAMemoryModel::classify(transfer.src);
        const MemorySpaceInfo dst_info =
            CUDAMemoryModel::classify(transfer.dst);
        transfer.src_space = src_info.space;
        transfer.dst_space = dst_info.space;
        transfer.kind = classifyTransferKind(src_info.space, dst_info.space);

        if (auto explicit_kind =
                getExplicitMemcpyKind(call, decoded.kind_index)) {
          transfer.kind = *explicit_kind;
          switch (*explicit_kind) {
          case TransferKind::HostToHost:
            transfer.src_space = MemorySpace::Host;
            transfer.dst_space = MemorySpace::Host;
            break;
          case TransferKind::HostToDevice:
            transfer.src_space = MemorySpace::Host;
            transfer.dst_space = MemorySpace::Device;
            break;
          case TransferKind::DeviceToHost:
            transfer.src_space = MemorySpace::Device;
            transfer.dst_space = MemorySpace::Host;
            break;
          case TransferKind::DeviceToDevice:
            transfer.src_space = MemorySpace::Device;
            transfer.dst_space = MemorySpace::Device;
            break;
          case TransferKind::Unknown:
            break;
          }
        }

        if (transfer.kind == TransferKind::Unknown) {
          const Function *callee = call->getCalledFunction();
          const StringRef name = callee ? callee->getName() : StringRef{};
          if (name.contains("ToSymbol")) {
            transfer.kind = TransferKind::HostToDevice;
            transfer.dst_space = MemorySpace::Constant;
          } else if (name.contains("FromSymbol")) {
            transfer.kind = TransferKind::DeviceToHost;
            transfer.src_space = MemorySpace::Constant;
          } else if (name.contains("Peer")) {
            transfer.kind = TransferKind::DeviceToDevice;
            transfer.src_space = MemorySpace::Device;
            transfer.dst_space = MemorySpace::Device;
          }
        }
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains("Peer")) {
          transfer.kind = TransferKind::DeviceToDevice;
          transfer.src_space = MemorySpace::Device;
          transfer.dst_space = MemorySpace::Device;
        }

        m_memory_transfers.push_back(transfer);

        CUDAMemoryTransferFact fact;
        fact.transfer_class_id = m_abstract_state.memory_transfer_facts.size();
        fact.inst = &inst;
        fact.src = transfer.src;
        fact.dst = transfer.dst;
        fact.stream = stream;
        fact.size = transfer.size;
        fact.kind = transfer.kind;
        fact.is_async = transfer.is_async;
        fact.stream_known = stream != nullptr;
        m_abstract_state.memory_transfer_facts.push_back(fact);
        m_abstract_state.transfer_fact_by_class[fact.transfer_class_id] = fact;

        if (transfer.kind == TransferKind::Unknown) {
          recordModelGap(&inst,
                         "CUDA memcpy-like operation has unknown "
                         "transfer direction because memory spaces "
                         "could not be classified precisely",
                         0.45);
        }
        if (transfer.region_unknown) {
          recordModelGap(&inst,
                         "CUDA multidimensional transfer region could "
                         "not be decoded; treating its device effect "
                         "as opaque",
                         0.35);
        }
      }

      if (type == ThreadAPI::TD_CUDA_MEMSET && call->arg_size() >= 3) {
        MemoryTransferInfo transfer;
        const Function *memset_callee = call->getCalledFunction();
        const StringRef memset_name =
            memset_callee ? memset_callee->getName() : StringRef{};
        transfer.inst = &inst;
        transfer.host_function = &function;
        transfer.dst = call->getArgOperand(0);
        transfer.src = nullptr;
        if (memset_name.contains("Memset3D")) {
          transfer.region_unknown = true;
        } else if (memset_name.contains("Memset2D") && call->arg_size() >= 5) {
          transfer.dst_pitch = getConstantExtent(call, 1, 0);
          transfer.width = getConstantExtent(call, 3, 0);
          transfer.height = getConstantExtent(call, 4, 1);
          if (transfer.height > 0 && transfer.dst_pitch > 0 &&
              transfer.height - 1 <=
                  (std::numeric_limits<uint64_t>::max() - transfer.width) /
                      transfer.dst_pitch) {
            transfer.size =
                (transfer.height - 1) * transfer.dst_pitch + transfer.width;
          } else {
            transfer.region_unknown = true;
          }
        } else {
          transfer.width = getConstantExtent(call, 2, 0);
          transfer.size = transfer.width;
        }
        transfer.dst_space = MemorySpace::Device;
        transfer.src_space = MemorySpace::Host;
        transfer.kind = TransferKind::HostToDevice;
        transfer.is_async =
            call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains("Async");
        const Value *stream = getTransferStream(call);
        transfer.stream = stream;
        transfer.stream_known = stream != nullptr;

        m_memory_transfers.push_back(transfer);

        CUDAMemoryTransferFact fact;
        fact.transfer_class_id = m_abstract_state.memory_transfer_facts.size();
        fact.inst = &inst;
        fact.src = nullptr;
        fact.dst = transfer.dst;
        fact.stream = stream;
        fact.size = transfer.size;
        fact.kind = transfer.kind;
        fact.is_async = transfer.is_async;
        fact.stream_known = stream != nullptr;
        m_abstract_state.memory_transfer_facts.push_back(fact);
        m_abstract_state.transfer_fact_by_class[fact.transfer_class_id] = fact;
      }

      if (type == ThreadAPI::TD_CUDA_MALLOC && call->arg_size() >= 1) {
        MemoryTransferInfo transfer;
        transfer.inst = &inst;
        transfer.host_function = &function;
        transfer.dst = call->getArgOperand(0);
        if (call->arg_size() >= 2) {
          if (auto size = evaluateConstantInt(call->getArgOperand(1))) {
            transfer.size = static_cast<uint64_t>(*size);
          }
        }
        const Function *callee = call->getCalledFunction();
        if (callee && callee->getName().contains("Managed")) {
          transfer.dst_space = MemorySpace::Unknown;
        } else {
          transfer.dst_space = MemorySpace::Device;
        }
        transfer.kind = TransferKind::HostToDevice;
        m_memory_transfers.push_back(transfer);

        CUDAMemoryTransferFact fact;
        fact.transfer_class_id = m_abstract_state.memory_transfer_facts.size();
        fact.inst = &inst;
        fact.dst = transfer.dst;
        if (call->arg_size() >= 2) {
          if (auto size = evaluateConstantInt(call->getArgOperand(1))) {
            fact.size = static_cast<uint64_t>(*size);
          }
        }
        fact.kind = transfer.kind;
        fact.is_async = false;
        m_abstract_state.memory_transfer_facts.push_back(fact);
        m_abstract_state.transfer_fact_by_class[fact.transfer_class_id] = fact;

        if (callee && callee->getName().contains("Managed")) {
          recordModelGap(&inst,
                         "Managed allocation is modeled conservatively "
                         "until unified-memory ownership is refined",
                         0.55);
        }
      }

      if (type == ThreadAPI::TD_CUDA_FREE && call->arg_size() >= 1) {
        MemoryTransferInfo transfer;
        transfer.inst = &inst;
        transfer.host_function = &function;
        transfer.src = call->getArgOperand(0);
        transfer.src_space = CUDAMemoryModel::classify(transfer.src).space;
        transfer.kind = TransferKind::DeviceToHost;
        m_memory_transfers.push_back(transfer);

        CUDAMemoryTransferFact fact;
        fact.transfer_class_id = m_abstract_state.memory_transfer_facts.size();
        fact.inst = &inst;
        fact.src = transfer.src;
        fact.kind = transfer.kind;
        fact.is_async = false;
        m_abstract_state.memory_transfer_facts.push_back(fact);
        m_abstract_state.transfer_fact_by_class[fact.transfer_class_id] = fact;
      }
    }
  }
}

void CUDAAnalysis::analyzeUnifiedMemory() {
  m_unified_memory.clear();

  for (Function &function : m_module) {
    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call ||
          m_thread_api->getType(call) != ThreadAPI::TD_CUDA_UNIFIED_MEMORY) {
        continue;
      }

      UnifiedMemoryInfo info;
      info.inst = &inst;

      const Function *callee = call->getCalledFunction();
      const StringRef name = callee ? callee->getName() : StringRef{};
      info.is_prefetch = name.contains("Prefetch");
      info.is_managed = name.contains("Managed");
      info.is_advise = name.contains("Advise");
      info.is_attach = name.contains("AttachMem");
      info.stream = getTransferStream(call);

      if (info.is_prefetch) {
        if (call->arg_size() >= 1) {
          info.ptr = call->getArgOperand(0);
        }
        if (call->arg_size() >= 2) {
          if (auto size = evaluateConstantInt(call->getArgOperand(1));
              size && *size > 0) {
            info.size = static_cast<uint64_t>(*size);
          }
        }
        if (call->arg_size() >= 3) {
          if (auto device = evaluateConstantInt(call->getArgOperand(2))) {
            info.device_id = static_cast<int>(*device);
          }
        }
        if (call->arg_size() >= 4) {
          info.stream = call->getArgOperand(3);
        }
      } else if (info.is_advise) {
        if (call->arg_size() >= 1) {
          info.ptr = call->getArgOperand(0);
        }
        if (call->arg_size() >= 2) {
          if (auto size = evaluateConstantInt(call->getArgOperand(1));
              size && *size > 0) {
            info.size = static_cast<uint64_t>(*size);
          }
        }
      } else if (info.is_attach) {
        if (call->arg_size() >= 1) {
          info.stream = call->getArgOperand(0);
        }
        if (call->arg_size() >= 2) {
          info.ptr = call->getArgOperand(1);
        }
      } else {
        if (call->arg_size() >= 1) {
          info.ptr = call->getArgOperand(0);
        }
        if (call->arg_size() >= 2) {
          if (auto size = evaluateConstantInt(call->getArgOperand(1));
              size && *size > 0) {
            info.size = static_cast<uint64_t>(*size);
          }
        }
      }

      m_unified_memory.push_back(info);

      if (info.ptr &&
          CUDAMemoryModel::classify(info.ptr).space == MemorySpace::Unknown &&
          !info.is_managed) {
        recordModelGap(&inst,
                       "Unified-memory operation references a pointer "
                       "with unknown memory-space classification",
                       0.4);
      }
    }
  }
}

void CUDAAnalysis::analyzeConstantAccesses(KernelSummary &summary) {
  for (const AccessInfo &access : summary.accesses) {
    if (access.space != MemorySpace::Constant) {
      continue;
    }

    ConstantAccessInfo info;
    info.inst = access.inst;
    info.base = access.base;
    info.access_size = access.access_size;

    if (access.address_pattern.valid) {
      info.strided = access.address_pattern.thread_idx_x > 1 ||
                     access.address_pattern.thread_idx_y > 1 ||
                     access.address_pattern.thread_idx_z > 1;
      info.stride = access.address_pattern.thread_idx_x +
                    access.address_pattern.thread_idx_y * 32 +
                    access.address_pattern.thread_idx_z * 32 * 32;
    }

    summary.constant_accesses.push_back(info);

    if (info.strided) {
      summary.has_uncoalesced_constant = true;
    }
  }
}

void CUDAAnalysis::analyzeTextureAndSurfaceAccesses(KernelSummary &summary) {
  if (!summary.kernel) {
    return;
  }

  for (const Instruction &inst : instructions(*summary.kernel)) {
    const auto *call = dyn_cast<CallBase>(&inst);
    if (!call) {
      continue;
    }
    ThreadAPI::TD_TYPE type = m_thread_api->getType(call);
    if (type == ThreadAPI::TD_CUDA_TEXTURE) {
      TextureAccessInfo info;
      info.inst = &inst;
      info.texref = call->arg_size() > 0 ? call->getArgOperand(0) : nullptr;
      info.dimensions = std::min<unsigned>(call->arg_size(), 3);
      summary.has_texture_access = true;
      summary.texture_accesses.push_back(info);
      recordModelGap(&inst,
                     "Texture resource binding and coordinates are not "
                     "yet normalized into byte-addressed race regions",
                     0.45);
    } else if (type == ThreadAPI::TD_CUDA_SURFACE) {
      SurfaceAccessInfo info;
      info.inst = &inst;
      info.surfref = call->arg_size() > 0 ? call->getArgOperand(0) : nullptr;
      info.dimensions = std::min<unsigned>(call->arg_size(), 3);
      const Function *callee = call->getCalledFunction();
      const StringRef name = callee ? callee->getName() : StringRef{};
      const bool is_read = name.contains_insensitive("read");
      const bool is_write = name.contains_insensitive("write");
      info.is_write = is_write;
      summary.has_surface_access = true;
      summary.surface_accesses.push_back(info);
      recordModelGap(&inst,
                     is_read || is_write
                         ? "Surface access direction is known, but resource "
                           "binding and coordinates are not normalized into "
                           "byte-addressed race regions"
                         : "Surface access direction and resource region are "
                           "not modeled precisely",
                     is_read || is_write ? 0.45 : 0.3);
    }
  }
}

} // namespace concurrency::cuda
