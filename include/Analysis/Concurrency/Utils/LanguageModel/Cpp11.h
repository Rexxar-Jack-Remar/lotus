#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>

namespace Cpp11Model {

// Check if the function is std::thread constructor (fork)
inline bool isFork(const llvm::StringRef& funcName) {
  // Mangled name for std::thread::thread matches
  // _ZNSt6threadC1IRFivEJEEEOT_DpOT0_ and similar patterns
  // We look for "std::thread::thread" in the demangled name or specific mangled patterns
  // A robust heuristic for mangled names: _ZNSt6threadC[12]
  return funcName.contains("_ZNSt6threadC");
}

// Check if the function is std::thread::join
inline bool isJoin(const llvm::StringRef& funcName) {
  return funcName.contains("_ZNSt6thread4joinEv");
}

// Check if the function is std::thread::detach
inline bool isDetach(const llvm::StringRef& funcName) {
  return funcName.contains("_ZNSt6thread6detachEv");
}

// Check if the function is std::mutex::lock or similar
inline bool isAcquire(const llvm::StringRef& funcName) {
  // std::mutex::lock -> _ZNSt5mutex4lockEv
  // std::recursive_mutex::lock -> _ZNSt15recursive_mutex4lockEv
  return funcName.contains("mutex") && funcName.contains("lockEv") && !funcName.contains("unlock");
}

// Check if the function is std::mutex::try_lock
inline bool isTryAcquire(const llvm::StringRef& funcName) {
    return funcName.contains("mutex") && funcName.contains("try_lockEv");
}

// Check if the function is std::mutex::unlock
inline bool isRelease(const llvm::StringRef& funcName) {
  return funcName.contains("mutex") && funcName.contains("unlockEv");
}

// Check if the function is std::condition_variable::wait
inline bool isCondWait(const llvm::StringRef& funcName) {
  return funcName.contains("condition_variable") && funcName.contains("wait");
}

// Check if the function is std::condition_variable::notify_one
inline bool isCondSignal(const llvm::StringRef& funcName) {
  return funcName.contains("condition_variable") && funcName.contains("notify_one");
}

// Check if the function is std::condition_variable::notify_all
inline bool isCondBroadcast(const llvm::StringRef& funcName) {
  return funcName.contains("condition_variable") && funcName.contains("notify_all");
}

} // namespace Cpp11Model

