#pragma once

#include <llvm/ADT/StringRef.h>

namespace CUDAModel {

inline bool isKernelLaunch(const llvm::StringRef &funcName) {
  return funcName.equals("__set_CUDAConfig") ||
         funcName.contains("cudaLaunchKernel") ||
         funcName.contains("cudaConfigureCall");
}

inline bool isDeviceSynchronize(const llvm::StringRef &funcName) {
  return funcName.equals("cudaDeviceSynchronize") ||
         funcName.equals("cudaThreadSynchronize");
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
         funcName.contains("cudaMemcpyAsync") || funcName.contains("memcpy");
}

inline bool isMemset(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMemset") ||
         funcName.contains("cudaMemsetAsync");
}

inline bool isMalloc(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMalloc") ||
         funcName.contains("cudaMallocManaged") ||
         funcName.contains("cudaMallocHost");
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
         funcName.contains("cudaMemPrefetchAsync");
}

inline bool isDeviceManagement(const llvm::StringRef &funcName) {
  return funcName.contains("cudaGetDevice") ||
         funcName.contains("cudaSetDevice") ||
         funcName.contains("cudaGetDeviceCount") ||
         funcName.contains("cudaGetDeviceProperties");
}

inline bool isErrorHandling(const llvm::StringRef &funcName) {
  return funcName.contains("cudaGetLastError") ||
         funcName.contains("cudaPeekAtLastError") ||
         funcName.contains("cudaGetErrorString");
}

} // namespace CUDAModel
