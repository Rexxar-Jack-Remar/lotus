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
    {TD::TD_CUDA_FREE, CUDAEffectKind::Free,
     CUDASemanticFamily::MemoryManagement, -1, -1, -1, 0, -1},
    {TD::TD_CUDA_UNIFIED_MEMORY, CUDAEffectKind::Unknown,
     CUDASemanticFamily::MemoryManagement, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_STREAM, CUDAEffectKind::Unknown,
     CUDASemanticFamily::StreamOperation, -1, -1, -1, -1, -1},
    {TD::TD_CUDA_EVENT, CUDAEffectKind::Unknown,
     CUDASemanticFamily::EventOperation, -1, -1, -1, -1, -1},
};

constexpr CUDASemanticDescriptor kStreamCreate = {
    TD::TD_CUDA_STREAM,
    CUDAEffectKind::StreamCreate,
    CUDASemanticFamily::StreamOperation,
    -1,
    -1,
    -1,
    0,
    -1};
constexpr CUDASemanticDescriptor kStreamDestroy = {
    TD::TD_CUDA_STREAM,
    CUDAEffectKind::StreamDestroy,
    CUDASemanticFamily::StreamOperation,
    0,
    -1,
    -1,
    -1,
    -1};
constexpr CUDASemanticDescriptor kStreamSync = {
    TD::TD_CUDA_STREAM,
    CUDAEffectKind::StreamSync,
    CUDASemanticFamily::StreamOperation,
    0,
    -1,
    -1,
    -1,
    -1};
constexpr CUDASemanticDescriptor kEventCreate = {
    TD::TD_CUDA_EVENT,
    CUDAEffectKind::EventCreate,
    CUDASemanticFamily::EventOperation,
    -1,
    -1,
    -1,
    0,
    -1};
constexpr CUDASemanticDescriptor kEventRecord = {
    TD::TD_CUDA_EVENT,
    CUDAEffectKind::EventRecord,
    CUDASemanticFamily::EventOperation,
    1,
    -1,
    -1,
    -1,
    0};
constexpr CUDASemanticDescriptor kEventWait = {
    TD::TD_CUDA_EVENT,
    CUDAEffectKind::EventWait,
    CUDASemanticFamily::EventOperation,
    0,
    -1,
    -1,
    -1,
    1};
constexpr CUDASemanticDescriptor kEventSync = {
    TD::TD_CUDA_EVENT,
    CUDAEffectKind::EventSynchronize,
    CUDASemanticFamily::EventOperation,
    -1,
    -1,
    -1,
    -1,
    0};
constexpr CUDASemanticDescriptor kEventDestroy = {
    TD::TD_CUDA_EVENT,
    CUDAEffectKind::EventDestroy,
    CUDASemanticFamily::EventOperation,
    -1,
    -1,
    -1,
    -1,
    0};
constexpr CUDASemanticDescriptor kPrefetch = {
    TD::TD_CUDA_UNIFIED_MEMORY,
    CUDAEffectKind::PrefetchAsync,
    CUDASemanticFamily::MemoryTransfer,
    3,
    2,
    1,
    -1,
    0};
constexpr CUDASemanticDescriptor kUnifiedMalloc = {
    TD::TD_CUDA_UNIFIED_MEMORY,
    CUDAEffectKind::UnifiedMalloc,
    CUDASemanticFamily::MemoryManagement,
    -1,
    -1,
    1,
    0,
    -1};

} // anonymous namespace

const CUDASemanticDescriptor *lookupCUDASemantic(ThreadAPI::TD_TYPE type) {
  for (const auto &desc : kDescriptors) {
    if (desc.type == type) {
      return &desc;
    }
  }
  return nullptr;
}

const CUDASemanticDescriptor *lookupCUDASemantic(const llvm::CallBase *call) {
  if (!call) {
    return nullptr;
  }
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api || api->getRuntimeLibrary(call) != ThreadAPI::RuntimeLibrary::CUDA) {
    return nullptr;
  }
  const CUDASemanticDescriptor *family = lookupCUDASemantic(api->getType(call));
  const llvm::Function *callee = call->getCalledFunction();
  if (!callee) {
    return family;
  }
  llvm::StringRef name = callee->getName();
  if (name.contains("StreamCreate")) {
    return &kStreamCreate;
  }
  if (name.contains("StreamDestroy")) {
    return &kStreamDestroy;
  }
  if (name.contains("StreamSynchronize")) {
    return &kStreamSync;
  }
  if (name.contains("StreamWaitEvent")) {
    return &kEventWait;
  }
  if (name.contains("EventCreate")) {
    return &kEventCreate;
  }
  if (name.contains("EventDestroy")) {
    return &kEventDestroy;
  }
  if (name.contains("EventRecord")) {
    return &kEventRecord;
  }
  if (name.contains("EventSynchronize")) {
    return &kEventSync;
  }
  if (name.contains("MemPrefetchAsync")) {
    return &kPrefetch;
  }
  if (name.contains("MallocManaged")) {
    return &kUnifiedMalloc;
  }
  return family;
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
  case CUDAEffectKind::EventDestroy:
    return "EventDestroy";
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
         type == ThreadAPI::TD_CUDA_MALLOC || type == ThreadAPI::TD_CUDA_FREE ||
         type == ThreadAPI::TD_CUDA_UNIFIED_MEMORY;
}

} // namespace concurrency::cuda
