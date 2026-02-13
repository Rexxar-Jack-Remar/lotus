/*
 *
 * Author: rainoftime
*/
#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

HappensBeforeAnalysis::HappensBeforeAnalysis(Module &module, mhp::MHPAnalysis &mhp)
    : m_module(module), m_mhp(mhp) {}

void HappensBeforeAnalysis::analyze() {
  buildSynchronizesWith();
}

void HappensBeforeAnalysis::buildSynchronizesWith() {
  m_sync_with.clear();
  using namespace CppAtomics;

  std::vector<const Instruction *> release_ops;
  std::vector<const Instruction *> acquire_ops;
  std::vector<const Instruction *> promise_sets;
  std::vector<const Instruction *> future_gets;
  std::vector<const Instruction *> call_once_ops;
  std::vector<const Instruction *> latch_countdowns;
  std::vector<const Instruction *> latch_waits;
  std::vector<const Instruction *> barrier_arrives;
  std::vector<const Instruction *> barrier_waits;

  ThreadAPI *threadAPI = ThreadAPI::getThreadAPI();

  for (Function &F : m_module) {
    if (F.isDeclaration()) continue;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;
      
      // Atomic operations with release/acquire semantics
      if (isAtomic(inst)) {
        if (hasReleaseSemantics(inst) && (isStore(inst) || isReadModifyWrite(inst)))
          release_ops.push_back(inst);
        if (hasAcquireSemantics(inst) && (isLoad(inst) || isReadModifyWrite(inst)))
          acquire_ops.push_back(inst);
      }
      
      // Fence instructions
      if (isFenceRelease(inst) || isFenceAcqRel(inst) || isFenceSeqCst(inst))
        release_ops.push_back(inst);
      if (isFenceAcquire(inst) || isFenceAcqRel(inst) || isFenceSeqCst(inst))
        acquire_ops.push_back(inst);
      
      // C++11 future/promise synchronization
      const CallInst *call = dyn_cast<CallInst>(inst);
      if (call && call->getCalledFunction()) {
        ThreadAPI::TD_TYPE type = threadAPI->getType(call->getCalledFunction());
        
        switch (type) {
          case ThreadAPI::TD_PROMISE_SET:
            promise_sets.push_back(inst);
            break;
          case ThreadAPI::TD_FUTURE_GET:
          case ThreadAPI::TD_FUTURE_WAIT:
            future_gets.push_back(inst);
            break;
          case ThreadAPI::TD_CALL_ONCE:
            call_once_ops.push_back(inst);
            break;
          case ThreadAPI::TD_LATCH_COUNT_DOWN:
          case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
            latch_countdowns.push_back(inst);
            break;
          case ThreadAPI::TD_LATCH_WAIT:
            latch_waits.push_back(inst);
            break;
          case ThreadAPI::TD_BARRIER_ARRIVE:
          case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
            barrier_arrives.push_back(inst);
            if (type == ThreadAPI::TD_BARRIER_ARRIVE_WAIT)
              barrier_waits.push_back(inst); // Acts as both
            break;
          case ThreadAPI::TD_BARRIER_WAIT_CPP20:
            barrier_waits.push_back(inst);
            break;
          default:
            break;
        }
      }
    }
  }

  // 1. Atomic release-acquire pairs
  for (const Instruction *R : release_ops) {
    for (const Instruction *A : acquire_ops) {
      if (R == A) continue;
      if (!isFence(R) && !isFence(A) && sameAtomicLocation(R, A))
        m_sync_with.emplace_back(R, A);
      else if (isFence(R) || isFence(A))
        // Fences create more conservative synchronization
        m_sync_with.emplace_back(R, A);
    }
  }

  // 2. Promise-Future synchronization
  // promise.set_value() synchronizes-with future.get()
  for (const Instruction *P : promise_sets) {
    for (const Instruction *F : future_gets) {
      if (P == F) continue;
      if (samePromiseFuturePair(P, F))
        m_sync_with.emplace_back(P, F);
    }
  }

  // 3. call_once synchronization
  // First call_once execution synchronizes-with all subsequent ones
  for (size_t i = 0; i < call_once_ops.size(); ++i) {
    for (size_t j = i + 1; j < call_once_ops.size(); ++j) {
      if (sameOnceFlag(call_once_ops[i], call_once_ops[j])) {
        // Conservative: assume any ordering
        m_sync_with.emplace_back(call_once_ops[i], call_once_ops[j]);
        m_sync_with.emplace_back(call_once_ops[j], call_once_ops[i]);
      }
    }
  }

  // 4. Latch synchronization
  // count_down synchronizes-with wait
  for (const Instruction *C : latch_countdowns) {
    for (const Instruction *W : latch_waits) {
      if (C == W) continue;
      if (sameLatch(C, W))
        m_sync_with.emplace_back(C, W);
    }
  }

  // 5. Barrier synchronization
  // All arrives synchronize-with all waits at same barrier
  for (const Instruction *A : barrier_arrives) {
    for (const Instruction *W : barrier_waits) {
      if (A == W) continue;
      if (sameBarrier(A, W))
        m_sync_with.emplace_back(A, W);
    }
  }
}

bool HappensBeforeAnalysis::sameAtomicLocation(const llvm::Instruction *store_inst,
                                                const llvm::Instruction *load_inst) const {
  const Value *p1 = CppAtomics::getAtomicPointer(store_inst);
  const Value *p2 = CppAtomics::getAtomicPointer(load_inst);
  if (!p1 || !p2) return false;
  if (p1->stripPointerCasts() == p2->stripPointerCasts()) return true;
  if (m_alias_analysis && m_alias_analysis->mayAlias(p1, p2)) return true;
  return false;
}

bool HappensBeforeAnalysis::samePromiseFuturePair(const llvm::Instruction *promise,
                                                   const llvm::Instruction *future) const {
  // Check if promise and future operate on the same shared state
  // This is conservative: we assume they might be paired if we can't prove otherwise
  const CallInst *p = dyn_cast<CallInst>(promise);
  const CallInst *f = dyn_cast<CallInst>(future);
  if (!p || !f) return false;
  
  // Extract the promise/future objects (typically first argument is 'this')
  if (p->arg_size() == 0 || f->arg_size() == 0) return false;
  
  const Value *p_obj = p->getArgOperand(0)->stripPointerCasts();
  const Value *f_obj = f->getArgOperand(0)->stripPointerCasts();
  
  // Conservative: assume they might be paired
  if (m_alias_analysis && m_alias_analysis->mayAlias(p_obj, f_obj))
    return true;
  
  // Check if they're derived from the same allocation
  if (p_obj == f_obj) return true;
  
  return false; // Conservative: assume not paired if we can't tell
}

bool HappensBeforeAnalysis::sameOnceFlag(const llvm::Instruction *call1,
                                          const llvm::Instruction *call2) const {
  const CallInst *c1 = dyn_cast<CallInst>(call1);
  const CallInst *c2 = dyn_cast<CallInst>(call2);
  if (!c1 || !c2) return false;
  
  // call_once takes once_flag as first argument
  if (c1->arg_size() < 1 || c2->arg_size() < 1) return false;
  
  const Value *flag1 = c1->getArgOperand(0)->stripPointerCasts();
  const Value *flag2 = c2->getArgOperand(0)->stripPointerCasts();
  
  if (flag1 == flag2) return true;
  if (m_alias_analysis && m_alias_analysis->mustAlias(flag1, flag2)) return true;
  
  return false;
}

bool HappensBeforeAnalysis::sameLatch(const llvm::Instruction *inst1,
                                       const llvm::Instruction *inst2) const {
  const CallInst *c1 = dyn_cast<CallInst>(inst1);
  const CallInst *c2 = dyn_cast<CallInst>(inst2);
  if (!c1 || !c2) return false;
  
  // Latch operations take latch object as first argument (this pointer)
  if (c1->arg_size() < 1 || c2->arg_size() < 1) return false;
  
  const Value *latch1 = c1->getArgOperand(0)->stripPointerCasts();
  const Value *latch2 = c2->getArgOperand(0)->stripPointerCasts();
  
  if (latch1 == latch2) return true;
  if (m_alias_analysis && m_alias_analysis->mustAlias(latch1, latch2)) return true;
  
  return false;
}

bool HappensBeforeAnalysis::sameBarrier(const llvm::Instruction *inst1,
                                         const llvm::Instruction *inst2) const {
  const CallInst *c1 = dyn_cast<CallInst>(inst1);
  const CallInst *c2 = dyn_cast<CallInst>(inst2);
  if (!c1 || !c2) return false;
  
  // Barrier operations take barrier object as first argument (this pointer)
  if (c1->arg_size() < 1 || c2->arg_size() < 1) return false;
  
  const Value *barrier1 = c1->getArgOperand(0)->stripPointerCasts();
  const Value *barrier2 = c2->getArgOperand(0)->stripPointerCasts();
  
  if (barrier1 == barrier2) return true;
  if (m_alias_analysis && m_alias_analysis->mustAlias(barrier1, barrier2)) return true;
  
  return false;
}

bool HappensBeforeAnalysis::happensBefore(const Instruction *A, const Instruction *B) const {
  if (!A || !B) return false;
  if (A == B) return true;

  auto key = std::make_pair(A, B);
  if (m_hb_cache.count(key))
    return m_hb_cache[key];

  bool result = m_mhp.mustPrecede(A, B);

  if (!result) {
    for (const auto &p : m_sync_with) {
      const Instruction *S = p.first;
      const Instruction *L = p.second;
      if (m_mhp.mustPrecede(A, S) && m_mhp.mustPrecede(L, B)) {
        result = true;
        break;
      }
    }
  }

  m_hb_cache[key] = result;
  return result;
}

} // namespace lotus

