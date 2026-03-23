/*
 *
 * Author: rainoftime
 */
#include "Analysis/Concurrency/Memory/EscapeAnalysis.h"

#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <deque>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

namespace {

const Function *resolveInternalCallee(const CallBase *call) {
  if (!call) {
    return nullptr;
  }
  if (Function *direct = call->getCalledFunction()) {
    return direct;
  }
  return dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
}

void collectLocalCarrierLoads(const Value *slot_base,
                              std::vector<const Value *> &loads) {
  if (!slot_base) {
    return;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(slot_base->stripPointerCasts());
  visited.insert(slot_base->stripPointerCasts());

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();

    for (const User *user : current->users()) {
      const Value *derived = dyn_cast<Value>(user);
      if (!derived || !visited.insert(derived).second) {
        continue;
      }

      if (const auto *load = dyn_cast<LoadInst>(user)) {
        if (load->getPointerOperand()->stripPointerCasts() == current) {
          loads.push_back(load);
        }
        continue;
      }

      if (isa<BitCastInst>(user) || isa<GetElementPtrInst>(user) ||
          isa<PHINode>(user) || isa<SelectInst>(user)) {
        worklist.push_back(derived);
      }
    }
  }
}

void collectLocalCarrierStoredValues(const Value *slot_base,
                                     std::vector<const Value *> &values) {
  if (!slot_base) {
    return;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(slot_base->stripPointerCasts());
  visited.insert(slot_base->stripPointerCasts());

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();

    for (const User *user : current->users()) {
      const Value *derived = dyn_cast<Value>(user);
      if (!derived || !visited.insert(derived).second) {
        continue;
      }

      if (const auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand()->stripPointerCasts() == current) {
          values.push_back(store->getValueOperand());
        }
        continue;
      }

      if (isa<BitCastInst>(user) || isa<GetElementPtrInst>(user) ||
          isa<PHINode>(user) || isa<SelectInst>(user)) {
        worklist.push_back(derived);
      }
    }
  }
}

} // namespace

EscapeAnalysis::EscapeAnalysis(Module &module) : m_module(module) {}

void EscapeAnalysis::analyze() {
  m_escaped_values.clear();
  m_visited.clear();
  runEscapeAnalysis();
}

bool EscapeAnalysis::isEscaped(const Value *val) const {
  if (!val)
    return false;
  // Globals are always escaped (shared)
  if (isa<GlobalValue>(val))
    return true;
  if (m_escaped_values.count(val)) {
    return true;
  }
  const Value *stripped = val->stripPointerCasts();
  if (m_escaped_values.count(stripped)) {
    return true;
  }
  if (const Value *root = getUnderlyingObject(stripped)) {
    return m_escaped_values.count(root->stripPointerCasts()) != 0;
  }
  return false;
}

bool EscapeAnalysis::isThreadLocal(const Value *val) const {
  return !isEscaped(val);
}

void EscapeAnalysis::runEscapeAnalysis() {
  std::vector<const Value *> worklist;
  auto enqueueEscaped = [&](const Value *value) {
    if (!value) {
      return;
    }

    std::vector<const Value *> candidates;
    candidates.push_back(value);
    const Value *stripped = value->stripPointerCasts();
    if (stripped != value) {
      candidates.push_back(stripped);
    }
    if (const auto *gep = dyn_cast<GetElementPtrInst>(stripped)) {
      candidates.push_back(gep->getPointerOperand()->stripPointerCasts());
    }
    if (const Value *root = getUnderlyingObject(stripped)) {
      candidates.push_back(root->stripPointerCasts());
    }

    for (const Value *candidate : candidates) {
      if (candidate && m_escaped_values.insert(candidate).second) {
        worklist.push_back(candidate);
      }
    }
  };

  // 1. Identify sources of escape
  // - Global variables
  // - Arguments to thread creation functions (pthread_create)

  for (const GlobalValue &gv : m_module.globals()) {
    enqueueEscaped(&gv);
  }

  auto *threadAPI = ThreadAPI::getThreadAPI();

  for (Function &F : m_module) {
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      Instruction *inst = &*I;

      // Check for thread creation
      if (threadAPI->isTDFork(inst)) {
        for (const Value *arg : threadAPI->getForkPayloadArgs(inst)) {
          if (!arg || !arg->getType()->isPointerTy()) {
            continue;
          }

          Type *pointee = arg->getType()->getPointerElementType();
          if (pointee && pointee->isFunctionTy()) {
            continue;
          }

          enqueueEscaped(arg);
        }
      }

      // Check for stores to escaped values
      if (auto *store = dyn_cast<StoreInst>(inst)) {
        Value *ptr = store->getPointerOperand();
        Value *val = store->getValueOperand();

        // If we store a value into an escaped pointer, the value escapes
        if (isEscaped(ptr)) {
          enqueueEscaped(val);
        }
      } else if (auto *call = dyn_cast<CallBase>(inst)) {
        Function *callee = call->getCalledFunction();
        if (!callee) {
          callee =
              dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
        }

        if (threadAPI->isTDFork(inst)) {
          continue;
        }

        for (unsigned arg_idx = 0; arg_idx < call->arg_size(); ++arg_idx) {
          const Value *arg = call->getArgOperand(arg_idx);
          if (!arg || !arg->getType()->isPointerTy()) {
            continue;
          }

          bool escapes_via_call = false;
          if (!callee || (callee->isDeclaration() && !callee->isIntrinsic())) {
            escapes_via_call = !call->doesNotCapture(arg_idx);
          }

          if (escapes_via_call) {
            enqueueEscaped(arg);
          }
        }
      }
    }
  }

  // 2. Propagate escape status
  while (!worklist.empty()) {
    const Value *curr = worklist.back();
    worklist.pop_back();

    if (const auto *load = dyn_cast<LoadInst>(curr)) {
      const Value *slot_base = getUnderlyingObject(load->getPointerOperand());
      slot_base = slot_base ? slot_base->stripPointerCasts()
                            : load->getPointerOperand()->stripPointerCasts();
      if (isa<AllocaInst>(slot_base)) {
        std::vector<const Value *> stored_values;
        collectLocalCarrierStoredValues(slot_base, stored_values);
        for (const Value *stored : stored_values) {
          enqueueEscaped(stored);
        }
      }
    }

    // Handle Formal Argument -> Actual Argument (Callers)
    if (auto *arg = dyn_cast<Argument>(curr)) {
      const Function *F = arg->getParent();
      unsigned argNo = arg->getArgNo();
      for (const User *U : F->users()) {
        if (auto *CB = dyn_cast<CallBase>(U)) {
          if (resolveInternalCallee(CB) == F) {
            const Value *actualArg = CB->getArgOperand(argNo);
            enqueueEscaped(actualArg);
          }
        }
      }
      if (F) {
        for (const Function &caller : m_module) {
          for (const BasicBlock &bb : caller) {
            for (const Instruction &inst : bb) {
              const auto *CB = dyn_cast<CallBase>(&inst);
              if (!CB || argNo >= CB->arg_size()) {
                continue;
              }
              if (resolveInternalCallee(CB) != F) {
                continue;
              }
              const Value *actualArg = CB->getArgOperand(argNo);
              enqueueEscaped(actualArg);
            }
          }
        }
      }
    }

    // Handle CallSite Result -> Callee Return Value only when the call site
    // itself escapes (e.g. result stored to global or passed to thread). This
    // avoids over-escaping when the caller does not expose the return value.
    if (auto *CB = dyn_cast<CallBase>(curr)) {
      if (!m_escaped_values.count(CB))
        continue;
      const Function *callee = resolveInternalCallee(CB);
      if (callee && !callee->isDeclaration()) {
        for (unsigned arg_idx = 0; arg_idx < CB->arg_size(); ++arg_idx) {
          const Value *actual_arg = CB->getArgOperand(arg_idx);
          if (!actual_arg || !actual_arg->getType()->isPointerTy()) {
            continue;
          }
          enqueueEscaped(actual_arg);
        }
        for (const BasicBlock &BB : *callee) {
          if (auto *ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
            if (const Value *retVal = ret->getReturnValue()) {
              enqueueEscaped(retVal);
            }
          }
        }
      }
    }

    // Find all uses of this escaped value
    for (const Use &U : curr->uses()) {
      const User *user = U.getUser();

      if (auto *inst = dyn_cast<Instruction>(user)) {
        // If used in a store as the value being stored, the pointer doesn't
        // necessarily escape But if used as the pointer, the value stored
        // escapes (handled above/below)

        if (auto *store = dyn_cast<StoreInst>(inst)) {
          if (store->getValueOperand() == curr) {
            const Value *slot_ptr = store->getPointerOperand();
            const Value *slot_base = getUnderlyingObject(slot_ptr);
            slot_base = slot_base ? slot_base->stripPointerCasts()
                                  : slot_ptr->stripPointerCasts();

            // Forward escape through local pointer carrier slots without
            // marking the carrier storage itself as shared.
            if (isa<AllocaInst>(slot_base)) {
              std::vector<const Value *> forwarded_loads;
              collectLocalCarrierLoads(slot_base, forwarded_loads);
              for (const Value *load : forwarded_loads) {
                enqueueEscaped(load);
              }
            } else if (isEscaped(slot_ptr)) {
              // Already handled via escaped destination storage.
            }
          } else if (store->getPointerOperand() == curr) {
            // Storing into an escaped pointer -> value escapes
            const Value *val = store->getValueOperand();
            enqueueEscaped(val);
          }
        } else if (auto *load = dyn_cast<LoadInst>(inst)) {
          // Loading from an escaped pointer -> result escapes
          enqueueEscaped(load);
          const Value *slot_base = getUnderlyingObject(load->getPointerOperand());
          slot_base = slot_base ? slot_base->stripPointerCasts()
                                : load->getPointerOperand()->stripPointerCasts();
          if (isa<AllocaInst>(slot_base)) {
            std::vector<const Value *> stored_values;
            collectLocalCarrierStoredValues(slot_base, stored_values);
            for (const Value *stored : stored_values) {
              enqueueEscaped(stored);
            }
          }
        } else if (auto *gep = dyn_cast<GetElementPtrInst>(inst)) {
          // GEP of escaped pointer -> result escapes
          enqueueEscaped(gep);
        } else if (auto *cast = dyn_cast<CastInst>(inst)) {
          // Cast of escaped value -> result escapes
          enqueueEscaped(cast);
        } else if (auto *phi = dyn_cast<PHINode>(inst)) {
          // PHI node with escaped operand -> result escapes
          enqueueEscaped(phi);
        } else if (auto *select = dyn_cast<SelectInst>(inst)) {
          // Select with escaped operand -> result escapes
          enqueueEscaped(select);
        } else if (auto *call = dyn_cast<CallBase>(inst)) {
          // Propagate from Actual Argument -> Formal Argument
          const Function *callee = resolveInternalCallee(call);
          if (callee && !callee->isDeclaration()) {
            for (unsigned i = 0; i < call->arg_size(); ++i) {
              if (call->getArgOperand(i) == curr) {
                if (i < callee->arg_size()) {
                  Argument *formalArg =
                      const_cast<Function *>(callee)->getArg(i);
                  enqueueEscaped(formalArg);
                }
              }
            }
          }
        } else if (auto *ret = dyn_cast<ReturnInst>(inst)) {
          // Propagate from Return Value -> Call Site
          const Function *F = ret->getFunction();
          for (const User *U : F->users()) {
            if (auto *CB = dyn_cast<CallBase>(U)) {
              if (resolveInternalCallee(CB) == F) {
                enqueueEscaped(CB);
              }
            }
          }
        }
      }
    }
  }
}

} // namespace lotus
