/**
 * @file Cpp11Atomics.cpp
 * @brief C++11 Atomics Recognition Implementation
 *
 * @author rainofetime
 * @date 2025-2026
 */

#include "Analysis/Concurrency/MHP/Cpp11Atomics.h"
#include <llvm/IR/Instructions.h>

namespace Cpp11Atomics {

// Helper to convert LLVM's AtomicOrdering to our enum
static MemoryOrder fromLLVMOrdering(llvm::AtomicOrdering ordering) {
    switch (ordering) {
        case llvm::AtomicOrdering::NotAtomic:
            return MemoryOrder::NotAtomic;
        case llvm::AtomicOrdering::Unordered: // Map Unordered to Relaxed
        case llvm::AtomicOrdering::Monotonic: // Map Monotonic to Relaxed
            return MemoryOrder::Relaxed;
        case llvm::AtomicOrdering::Acquire:
            return MemoryOrder::Acquire;
        case llvm::AtomicOrdering::Release:
            return MemoryOrder::Release;
        case llvm::AtomicOrdering::AcquireRelease:
            return MemoryOrder::AcquireRelease;
        case llvm::AtomicOrdering::SequentiallyConsistent:
            return MemoryOrder::SequentiallyConsistent;
        default:
            return MemoryOrder::NotAtomic;
    }
}

bool isAtomic(const llvm::Instruction *inst) {
    if (!inst) return false;
    return inst->isAtomic();
}

MemoryOrder getMemoryOrder(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) {
        return MemoryOrder::NotAtomic;
    }

    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        return fromLLVMOrdering(load->getOrdering());
    }
    if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        return fromLLVMOrdering(store->getOrdering());
    }
    if (const auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(inst)) {
        return fromLLVMOrdering(rmw->getOrdering());
    }
    if (const auto *cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst)) {
        return fromLLVMOrdering(cmpxchg->getSuccessOrdering());
    }
    if (const auto *fence = llvm::dyn_cast<llvm::FenceInst>(inst)) {
        return fromLLVMOrdering(fence->getOrdering());
    }

    return MemoryOrder::NotAtomic;
}

const llvm::Value *getAtomicPointer(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) {
        return nullptr;
    }

    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        return load->getPointerOperand();
    }
    if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        return store->getPointerOperand();
    }
    if (const auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(inst)) {
        return rmw->getPointerOperand();
    }
    if (const auto *cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst)) {
        return cmpxchg->getPointerOperand();
    }
    
    // Fence instructions do not operate on a specific pointer
    return nullptr;
}

bool isLockFree(const llvm::Instruction *inst) {
    // This is a simplification. A proper implementation would check the size
    // and alignment of the atomic operation.
    if (const auto *cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst)) {
        return cmpxchg->isVolatile();
    }
    return false;
}

bool isStore(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) return false;
    return llvm::isa<llvm::StoreInst>(inst) || llvm::isa<llvm::AtomicRMWInst>(inst) || llvm::isa<llvm::AtomicCmpXchgInst>(inst);
}

bool isLoad(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) return false;
    return llvm::isa<llvm::LoadInst>(inst) || llvm::isa<llvm::AtomicRMWInst>(inst) || llvm::isa<llvm::AtomicCmpXchgInst>(inst);
}

} // namespace Cpp11Atomics
