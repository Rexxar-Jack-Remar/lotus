#include "Concurrency/CUDA/CUDAAnalysis.h"

#include <llvm/IR/InstIterator.h>

using namespace llvm;

namespace concurrency::cuda {

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
          type != ThreadAPI::TD_CUDA_MALLOC) {
        continue;
      }

      if (type == ThreadAPI::TD_CUDA_MEMCPY && call->arg_size() >= 3) {
        MemoryTransferInfo transfer;
        transfer.inst = &inst;
        transfer.dst = call->getArgOperand(0);
        transfer.src = call->getArgOperand(1);
        if (call->arg_size() >= 3) {
          if (auto size = evaluateConstantInt(call->getArgOperand(2))) {
            transfer.size = static_cast<uint64_t>(*size);
          }
        }
        transfer.is_async =
            call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains("Async");

        const MemorySpaceInfo src_info =
            CUDAMemoryModel::classify(transfer.src);
        const MemorySpaceInfo dst_info =
            CUDAMemoryModel::classify(transfer.dst);
        transfer.src_space = src_info.space;
        transfer.dst_space = dst_info.space;

        if (src_info.space == MemorySpace::Host &&
            dst_info.space == MemorySpace::Device) {
          transfer.kind = TransferKind::HostToDevice;
        } else if (src_info.space == MemorySpace::Device &&
                   dst_info.space == MemorySpace::Host) {
          transfer.kind = TransferKind::DeviceToHost;
        } else if (src_info.space == MemorySpace::Device &&
                   dst_info.space == MemorySpace::Device) {
          transfer.kind = TransferKind::DeviceToDevice;
        } else if (src_info.space == MemorySpace::Host &&
                   dst_info.space == MemorySpace::Host) {
          transfer.kind = TransferKind::HostToHost;
        }

        m_memory_transfers.push_back(transfer);

        CUDAMemoryTransferFact fact;
        fact.transfer_class_id = m_abstract_state.memory_transfer_facts.size();
        fact.inst = &inst;
        fact.src = transfer.src;
        fact.dst = transfer.dst;
        if (call->arg_size() >= 3) {
          if (auto size = evaluateConstantInt(call->getArgOperand(2))) {
            fact.size = static_cast<uint64_t>(*size);
          }
        }
        fact.kind = transfer.kind;
        fact.is_async = transfer.is_async;
        m_abstract_state.memory_transfer_facts.push_back(fact);
        m_abstract_state.transfer_fact_by_class[fact.transfer_class_id] = fact;
      }

      if (type == ThreadAPI::TD_CUDA_MEMSET && call->arg_size() >= 3) {
        MemoryTransferInfo transfer;
        transfer.inst = &inst;
        transfer.dst = call->getArgOperand(0);
        transfer.src = nullptr;
        if (call->arg_size() >= 3) {
          if (auto size = evaluateConstantInt(call->getArgOperand(2))) {
            transfer.size = static_cast<uint64_t>(*size);
          }
        }
        transfer.dst_space = MemorySpace::Device;
        transfer.src_space = MemorySpace::Host;
        transfer.kind = TransferKind::HostToDevice;
        transfer.is_async =
            call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains("Async");

        m_memory_transfers.push_back(transfer);

        CUDAMemoryTransferFact fact;
        fact.transfer_class_id = m_abstract_state.memory_transfer_facts.size();
        fact.inst = &inst;
        fact.src = nullptr;
        fact.dst = transfer.dst;
        if (call->arg_size() >= 3) {
          if (auto size = evaluateConstantInt(call->getArgOperand(2))) {
            fact.size = static_cast<uint64_t>(*size);
          }
        }
        fact.kind = transfer.kind;
        fact.is_async = transfer.is_async;
        m_abstract_state.memory_transfer_facts.push_back(fact);
        m_abstract_state.transfer_fact_by_class[fact.transfer_class_id] = fact;
      }

      if (type == ThreadAPI::TD_CUDA_MALLOC && call->arg_size() >= 1) {
        MemoryTransferInfo transfer;
        transfer.inst = &inst;
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

} // namespace concurrency::cuda
