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
  m_future_shared_state.clear();
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
      const CallBase *call = dyn_cast<CallBase>(inst);
      if (call && call->getCalledFunction()) {
        const Function *callee = call->getCalledFunction();
        ThreadAPI::TD_TYPE type = threadAPI->getType(callee);

        if (callee->getName().contains("get_future") && call->arg_size() >= 1) {
          const Value *promise_obj = traceSharedState(call->getArgOperand(0));
          if (promise_obj) {
            m_future_shared_state[call] = promise_obj;
          }
        }
        
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
        m_sync_with.emplace_back(call_once_ops[i], call_once_ops[j]);
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
  const CallBase *p = dyn_cast<CallBase>(promise);
  const CallBase *f = dyn_cast<CallBase>(future);
  if (!p || !f) return false;
  
  if (p->arg_size() == 0 || f->arg_size() == 0) return false;
  
  const Value *promise_state = traceSharedState(p->getArgOperand(0));
  const Value *future_state = traceSharedState(f->getArgOperand(0));
  if (!promise_state || !future_state) {
    return false;
  }

  if (promise_state == future_state) return true;
  if (m_alias_analysis && m_alias_analysis->mustAlias(promise_state, future_state))
    return true;
  
  return false;
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

const Value *HappensBeforeAnalysis::traceSharedState(const Value *value) const {
  if (!value) {
    return nullptr;
  }

  std::vector<const Value *> worklist = {value};
  std::unordered_set<const Value *> visited;

  while (!worklist.empty()) {
    const Value *current = worklist.back();
    worklist.pop_back();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    auto mapped = m_future_shared_state.find(current);
    if (mapped != m_future_shared_state.end()) {
      return mapped->second;
    }

    const Value *stripped = current->stripPointerCasts();
    if (stripped != current) {
      worklist.push_back(stripped);
    }

    if (isa<AllocaInst>(current) || isa<GlobalValue>(current)) {
      return current;
    }

    if (const auto *arg = dyn_cast<Argument>(current)) {
      const Function *parent = arg->getParent();
      bool expanded = false;
      if (parent) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (cb && arg->getArgNo() < cb->arg_size()) {
            worklist.push_back(cb->getArgOperand(arg->getArgNo()));
            expanded = true;
          }
        }

        for (const Function &func : m_module) {
          for (const BasicBlock &bb : func) {
            for (const Instruction &inst : bb) {
              const auto *cb = dyn_cast<CallBase>(&inst);
              if (!cb || arg->getArgNo() >= cb->arg_size()) {
                continue;
              }
              const Value *called = cb->getCalledOperand();
              if (called && called->stripPointerCasts() == parent) {
                worklist.push_back(cb->getArgOperand(arg->getArgNo()));
                expanded = true;
              }
            }
          }
        }
      }
      if (!expanded) {
        return current;
      }
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      for (const User *user : load->getPointerOperand()->users()) {
        if (const auto *store = dyn_cast<StoreInst>(user)) {
          if (store->getPointerOperand() == load->getPointerOperand()) {
            worklist.push_back(store->getValueOperand());
          }
        }
      }
    } else if (const auto *store = dyn_cast<StoreInst>(current)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
    } else if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
    } else if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
    } else if (const auto *cb = dyn_cast<CallBase>(current)) {
      if (cb->arg_size() >= 1) {
        worklist.push_back(cb->getArgOperand(0));
      }
    } else if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Use &operand : inst->operands()) {
        worklist.push_back(operand.get());
      }
    }
  }

  return nullptr;
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
