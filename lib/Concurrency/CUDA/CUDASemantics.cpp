#include "Concurrency/CUDA/CUDASemantics.h"

namespace concurrency::cuda {

namespace {

using TD = ThreadAPI::TD_TYPE;

constexpr CUDASemanticDescriptor kDescriptors[] = {
    {TD::TD_CUDA_KERNEL_LAUNCH, CUDAEffectKind::KernelLaunch,
     CUDASemanticFamily::KernelLaunch, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_DEVICE_SYNC, CUDAEffectKind::DeviceSync,
     CUDASemanticFamily::Synchronization, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_BARRIER, CUDAEffectKind::Barrier,
     CUDASemanticFamily::Synchronization, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_WARP_BARRIER, CUDAEffectKind::WarpBarrier,
     CUDASemanticFamily::Synchronization, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_MEMORY_BARRIER, CUDAEffectKind::MemoryFence,
     CUDASemanticFamily::Synchronization, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_ATOMIC, CUDAEffectKind::Atomic,
     CUDASemanticFamily::AtomicOperation, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_MEMCPY, CUDAEffectKind::Memcpy,
     CUDASemanticFamily::MemoryTransfer, -1, -1, 2, 0, 1},
    {TD::TD_CUDA_MEMSET, CUDAEffectKind::Memset,
     CUDASemanticFamily::MemoryTransfer, -1, -1, 2, 0, -1},
    {TD::TD_CUDA_MALLOC, CUDAEffectKind::Malloc,
     CUDASemanticFamily::MemoryManagement, -1, -1, 1, 0, -1},
    {TD::TD_CUDA_FREE, CUDAEffectKind::Free, CUDASemanticFamily::MemoryManagement,
     -1, -1, -1, 0, -1},
    {TD::TD_CUDA_UNIFIED_MEMORY, CUDAEffectKind::UnifiedMalloc,
     CUDASemanticFamily::MemoryManagement, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_STREAM, CUDAEffectKind::StreamSync,
     CUDASemanticFamily::StreamOperation, 0, -1, -1, -1, -1},
    {TD::TD_CUDA_EVENT, CUDAEffectKind::EventRecord,
     CUDASemanticFamily::EventOperation, 1, -1, -1, -1, -1},
};

} // anonymous namespace

const CUDASemanticDescriptor *lookupCUDASemantic(ThreadAPI::TD_TYPE type) {
  for (const auto &desc : kDescriptors) {
    if (desc.type == type) {
      return &desc;
    }
  }
  return nullptr;
}

const char *toString(CUDAEffectKind kind) {
  switch (kind) {
  case CUDAEffectKind::Unknown:
    return "Unknown";
  case CUDAEffectKind::KernelLaunch:
    return "KernelLaunch";
  case CUDAEffectKind::DeviceSync:
    return "DeviceSync";
  case CUDAEffectKind::StreamSync:
    return "StreamSync";
  case CUDAEffectKind::Barrier:
    return "Barrier";
  case CUDAEffectKind::WarpBarrier:
    return "WarpBarrier";
  case CUDAEffectKind::MemoryFence:
    return "MemoryFence";
  case CUDAEffectKind::Atomic:
    return "Atomic";
  case CUDAEffectKind::Memcpy:
    return "Memcpy";
  case CUDAEffectKind::Memset:
    return "Memset";
  case CUDAEffectKind::Malloc:
    return "Malloc";
  case CUDAEffectKind::Free:
    return "Free";
  case CUDAEffectKind::UnifiedMalloc:
    return "UnifiedMalloc";
  case CUDAEffectKind::UnifiedFree:
    return "UnifiedFree";
  case CUDAEffectKind::PrefetchAsync:
    return "PrefetchAsync";
  case CUDAEffectKind::StreamCreate:
    return "StreamCreate";
  case CUDAEffectKind::StreamDestroy:
    return "StreamDestroy";
  case CUDAEffectKind::EventCreate:
    return "EventCreate";
  case CUDAEffectKind::EventRecord:
    return "EventRecord";
  case CUDAEffectKind::EventWait:
    return "EventWait";
  case CUDAEffectKind::EventSynchronize:
    return "EventSynchronize";
  }
  return "Unknown";
}

const char *toString(CUDASemanticFamily family) {
  switch (family) {
  case CUDASemanticFamily::Unknown:
    return "Unknown";
  case CUDASemanticFamily::KernelLaunch:
    return "KernelLaunch";
  case CUDASemanticFamily::MemoryTransfer:
    return "MemoryTransfer";
  case CUDASemanticFamily::Synchronization:
    return "Synchronization";
  case CUDASemanticFamily::AtomicOperation:
    return "AtomicOperation";
  case CUDASemanticFamily::MemoryManagement:
    return "MemoryManagement";
  case CUDASemanticFamily::StreamOperation:
    return "StreamOperation";
  case CUDASemanticFamily::EventOperation:
    return "EventOperation";
  }
  return "Unknown";
}

bool isStreamOrderingOperation(ThreadAPI::TD_TYPE type) {
  return type == ThreadAPI::TD_CUDA_STREAM ||
         type == ThreadAPI::TD_CUDA_DEVICE_SYNC ||
         type == ThreadAPI::TD_CUDA_MEMORY_BARRIER;
}

bool isEventOperation(ThreadAPI::TD_TYPE type) {
  return type == ThreadAPI::TD_CUDA_EVENT;
}

bool isMemoryTransferOperation(ThreadAPI::TD_TYPE type) {
  return type == ThreadAPI::TD_CUDA_MEMCPY ||
         type == ThreadAPI::TD_CUDA_MEMSET ||
         type == ThreadAPI::TD_CUDA_MALLOC ||
         type == ThreadAPI::TD_CUDA_FREE ||
         type == ThreadAPI::TD_CUDA_UNIFIED_MEMORY;
}

} // namespace concurrency::cuda
