#pragma once

#include <llvm/ADT/StringRef.h>

#include <initializer_list>

namespace CUDAModel {

inline bool hasAnySubstring(const llvm::StringRef &funcName,
                            std::initializer_list<llvm::StringRef> needles) {
  for (const llvm::StringRef needle : needles) {
    if (funcName.contains(needle)) {
      return true;
    }
  }
  return false;
}

inline bool startsWithAny(const llvm::StringRef &funcName,
                          std::initializer_list<llvm::StringRef> prefixes) {
  for (const llvm::StringRef prefix : prefixes) {
    if (funcName.startswith(prefix)) {
      return true;
    }
  }
  return false;
}

inline bool isKernelLaunch(const llvm::StringRef &funcName) {
  return funcName.equals("__set_CUDAConfig") ||
         funcName.equals("__cudaPushCallConfiguration") ||
         funcName.equals("__cudaPopCallConfiguration") ||
         funcName.contains("cudaLaunchKernelEx") ||
         funcName.contains("cudaLaunchKernel") ||
         funcName.contains("cudaLaunchCooperativeKernel") ||
         funcName.contains("cudaGraphLaunch") ||
         funcName.contains("cudaGraphAddKernelNode") ||
         funcName.contains("cudaGraphExecKernelNodeSetParams") ||
         funcName.contains("cudaConfigureCall") ||
         startsWithAny(funcName, {"cuLaunch", "cuGraphLaunch"});
}

inline bool isDeviceSynchronize(const llvm::StringRef &funcName) {
  return funcName.equals("cudaDeviceSynchronize") ||
         funcName.equals("cudaThreadSynchronize") ||
         funcName.equals("cuCtxSynchronize");
}

inline bool isBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("llvm.nvvm.barrier0") ||
         funcName.equals("__syncthreads") ||
         funcName.equals("cuda.syncthreads");
}

inline bool isWarpBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("llvm.nvvm.bar.warp.sync") ||
         funcName.equals("__syncwarp");
}

inline bool isMemoryBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("llvm.nvvm.membar.cta") ||
         funcName.equals("llvm.nvvm.membar.gl") ||
         funcName.equals("llvm.nvvm.membar.sys") ||
         funcName.equals("__threadfence_block") ||
         funcName.equals("__threadfence") ||
         funcName.equals("__threadfence_system");
}

inline bool isAtomic(const llvm::StringRef &funcName) {
  return funcName.contains("atomic") ||
         funcName.startswith("llvm.nvvm.atomic.") ||
         funcName.startswith("llvm.nvvm.red.");
}

inline bool isMemcpy(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMemcpy") ||
         hasAnySubstring(funcName,
                         {"cudaGraphAddMemcpyNode",
                          "cudaGraphMemcpyNodeSetParams",
                          "cudaGraphExecMemcpyNodeSetParams"}) ||
         startsWithAny(funcName, {"cuMemcpy", "cuGraphMemcpyNode"}) ||
         funcName.contains("cudaMemPrefetchAsync") ||
         funcName.contains("memcpy");
}

inline bool isMemset(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMemset") ||
         startsWithAny(funcName, {"cuMemset"}) ||
         hasAnySubstring(funcName,
                         {"cudaGraphAddMemsetNode",
                          "cudaGraphMemsetNodeSetParams",
                          "cudaGraphExecMemsetNodeSetParams"});
}

inline bool isMalloc(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMalloc") ||
         funcName.contains("cudaMallocManaged") ||
         funcName.contains("cudaMallocHost") ||
         funcName.contains("cudaHostAlloc") ||
         hasAnySubstring(funcName,
                         {"cudaGraphAddMemAllocNode",
                          "cudaGraphMemAllocNodeGetParams"}) ||
         startsWithAny(funcName, {"cuMemAlloc", "cuArrayCreate",
                                  "cuMipmappedArrayCreate"});
}

inline bool isFree(const llvm::StringRef &funcName) {
  return funcName.contains("cudaFree") ||
         funcName.contains("cudaFreeHost") ||
         hasAnySubstring(funcName, {"cudaGraphAddMemFreeNode"}) ||
         startsWithAny(funcName, {"cuMemFree", "cuArrayDestroy",
                                  "cuMipmappedArrayDestroy"});
}

inline bool isStreamOperation(const llvm::StringRef &funcName) {
  return funcName.contains("cudaStreamCreate") ||
         funcName.contains("cudaStreamDestroy") ||
         funcName.contains("cudaStreamSynchronize") ||
         funcName.contains("cudaStreamWaitEvent") ||
         funcName.contains("cudaStreamQuery") ||
         funcName.contains("cudaStreamBeginCapture") ||
         funcName.contains("cudaStreamEndCapture") ||
         funcName.contains("cudaStreamIsCapturing") ||
         funcName.contains("cudaThreadExchangeStreamCaptureMode") ||
         startsWithAny(funcName, {"cuStreamCreate", "cuStreamDestroy",
                                  "cuStreamSynchronize", "cuStreamWaitEvent",
                                  "cuStreamQuery", "cuStreamBeginCapture",
                                  "cuStreamEndCapture",
                                  "cuStreamIsCapturing",
                                  "cuThreadExchangeStreamCaptureMode"});
}

inline bool isEventOperation(const llvm::StringRef &funcName) {
  return funcName.contains("cudaEventCreate") ||
         funcName.contains("cudaEventDestroy") ||
         funcName.contains("cudaEventRecord") ||
         funcName.contains("cudaEventSynchronize") ||
         funcName.contains("cudaEventQuery") ||
         funcName.contains("cudaEventElapsedTime") ||
         hasAnySubstring(funcName,
                         {"cudaGraphAddEventRecordNode",
                          "cudaGraphAddEventWaitNode",
                          "cudaGraphEventRecordNodeSetEvent",
                          "cudaGraphEventWaitNodeSetEvent",
                          "cudaGraphExecEventRecordNodeSetEvent",
                          "cudaGraphExecEventWaitNodeSetEvent"}) ||
         startsWithAny(funcName, {"cuEventCreate", "cuEventDestroy",
                                  "cuEventRecord", "cuEventSynchronize",
                                  "cuEventQuery", "cuEventElapsedTime"});
}

inline bool isTexture(const llvm::StringRef &funcName) {
  return funcName.contains("tex") || funcName.startswith("llvm.nvvm.tex");
}

inline bool isSurface(const llvm::StringRef &funcName) {
  return funcName.contains("surf") || funcName.startswith("llvm.nvvm.surf");
}

inline bool isUnifiedMemory(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMallocManaged") ||
         funcName.contains("cudaMallocHost") ||
         funcName.contains("cudaHostAlloc") ||
         funcName.contains("cudaMemPrefetchAsync") ||
         funcName.contains("cudaMemPrefetchBatchAsync") ||
         funcName.contains("cudaMemAdvise") ||
         funcName.contains("cudaMemRangeGetAttribute") ||
         funcName.contains("cudaMemRangeGetAttributes") ||
         funcName.contains("cudaStreamAttachMemAsync") ||
         startsWithAny(funcName, {"cuMemAllocManaged", "cuMemPrefetchAsync",
                                  "cuMemAdvise", "cuMemRangeGetAttribute"});
}

inline bool isDeviceManagement(const llvm::StringRef &funcName) {
  return funcName.contains("cudaGetDevice") ||
         funcName.contains("cudaSetDevice") ||
         funcName.contains("cudaGetDeviceCount") ||
         funcName.contains("cudaGetDeviceProperties") ||
         funcName.contains("cudaChooseDevice") ||
         funcName.contains("cudaSetValidDevices") ||
         funcName.contains("cudaDeviceGet") ||
         funcName.contains("cudaDeviceSet") ||
         funcName.contains("cudaDeviceReset") ||
         startsWithAny(funcName, {"cuInit", "cuDeviceGet", "cuDeviceGetCount",
                                  "cuCtxCreate", "cuCtxDestroy",
                                  "cuCtxSetCurrent", "cuCtxGetCurrent",
                                  "cuCtxPushCurrent", "cuCtxPopCurrent"});
}

inline bool isErrorHandling(const llvm::StringRef &funcName) {
  return funcName.contains("cudaGetLastError") ||
         funcName.contains("cudaPeekAtLastError") ||
         funcName.contains("cudaGetErrorString") ||
         funcName.contains("cudaGetErrorName") ||
         funcName.contains("cuGetErrorName") ||
         funcName.contains("cuGetErrorString");
}

} // namespace CUDAModel
