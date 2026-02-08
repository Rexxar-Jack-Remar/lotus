/*
 *
 * Author: rainoftime
*/
#include "Analysis/Concurrency/Memory/EscapeAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

EscapeAnalysis::EscapeAnalysis(Module &module) : m_module(module) {}

void EscapeAnalysis::analyze() {
  runEscapeAnalysis();
}

bool EscapeAnalysis::isEscaped(const Value *val) const {
  if (!val) return false;
  // Globals are always escaped (shared)
  if (isa<GlobalValue>(val)) return true;
  return m_escaped_values.count(val);
}

bool EscapeAnalysis::isThreadLocal(const Value *val) const {
  return !isEscaped(val);
}

void EscapeAnalysis::runEscapeAnalysis() {
  std::vector<const Value *> worklist;

  // 1. Identify sources of escape
  // - Global variables
  // - Arguments to thread creation functions (pthread_create)
  
  for (const GlobalValue &gv : m_module.globals()) {
    m_escaped_values.insert(&gv);
    worklist.push_back(&gv);
  }

  auto *threadAPI = ThreadAPI::getThreadAPI();

  for (Function &F : m_module) {
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      Instruction *inst = &*I;
      
      // Check for thread creation
      if (threadAPI->isTDFork(inst)) {
        // The argument passed to the thread function escapes
        if (const Value *arg = threadAPI->getActualParmAtForkSite(inst)) {
          if (m_escaped_values.insert(arg).second) {
            worklist.push_back(arg);
          }
        }
      }
      
      // Check for stores to escaped values
      if (auto *store = dyn_cast<StoreInst>(inst)) {
        Value *ptr = store->getPointerOperand();
        Value *val = store->getValueOperand();
        
        // If we store a value into an escaped pointer, the value escapes
        if (isEscaped(ptr)) {
           if (m_escaped_values.insert(val).second) {
             worklist.push_back(val);
           }
        }
      }
    }
  }

  // 2. Propagate escape status
  while (!worklist.empty()) {
    const Value *curr = worklist.back();
    worklist.pop_back();

    // Handle Formal Argument -> Actual Argument (Callers)
    if (auto *arg = dyn_cast<Argument>(curr)) {
      const Function *F = arg->getParent();
      unsigned argNo = arg->getArgNo();
      for (const User *U : F->users()) {
        if (auto *CB = dyn_cast<CallBase>(U)) {
          if (CB->getCalledFunction() == F) {
            const Value *actualArg = CB->getArgOperand(argNo);
            if (m_escaped_values.insert(actualArg).second) {
              worklist.push_back(actualArg);
            }
          }
        }
      }
    }

    // Handle CallSite Result -> Callee Return Value
    if (auto *CB = dyn_cast<CallBase>(curr)) {
       Function *callee = CB->getCalledFunction();
       if (callee && !callee->isDeclaration()) {
         for (BasicBlock &BB : *callee) {
           if (auto *ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
             if (Value *retVal = ret->getReturnValue()) {
               if (m_escaped_values.insert(retVal).second) {
                 worklist.push_back(retVal);
               }
             }
           }
         }
       }
    }

    // Find all uses of this escaped value
    for (const Use &U : curr->uses()) {
      const User *user = U.getUser();
      
      if (auto *inst = dyn_cast<Instruction>(user)) {
        // If used in a store as the value being stored, the pointer doesn't necessarily escape
        // But if used as the pointer, the value stored escapes (handled above/below)
        
        if (auto *store = dyn_cast<StoreInst>(inst)) {
          if (store->getValueOperand() == curr) {
             // Storing an escaped value into a pointer doesn't make the pointer escape
             // But storing INTO an escaped pointer makes the value escape (handled in initial scan + loop)
             const Value *ptr = store->getPointerOperand();
             // If we store an escaped value into a pointer, does the pointer escape? No.
             // But if we store a value into an escaped pointer, the value escapes.
             if (isEscaped(ptr)) {
                 // Already handled
             }
          } else if (store->getPointerOperand() == curr) {
             // Storing into an escaped pointer -> value escapes
             const Value *val = store->getValueOperand();
             if (m_escaped_values.insert(val).second) {
               worklist.push_back(val);
             }
          }
        } else if (auto *load = dyn_cast<LoadInst>(inst)) {
          // Loading from an escaped pointer -> result escapes
          if (m_escaped_values.insert(load).second) {
            worklist.push_back(load);
          }
        } else if (auto *gep = dyn_cast<GetElementPtrInst>(inst)) {
          // GEP of escaped pointer -> result escapes
          if (m_escaped_values.insert(gep).second) {
            worklist.push_back(gep);
          }
        } else if (auto *cast = dyn_cast<CastInst>(inst)) {
          // Cast of escaped value -> result escapes
          if (m_escaped_values.insert(cast).second) {
            worklist.push_back(cast);
          }
        } else if (auto *phi = dyn_cast<PHINode>(inst)) {
          // PHI node with escaped operand -> result escapes
          if (m_escaped_values.insert(phi).second) {
            worklist.push_back(phi);
          }
        } else if (auto *select = dyn_cast<SelectInst>(inst)) {
          // Select with escaped operand -> result escapes
          if (m_escaped_values.insert(select).second) {
            worklist.push_back(select);
          }
        } else if (auto *call = dyn_cast<CallBase>(inst)) {
           // Propagate from Actual Argument -> Formal Argument
           Function *callee = call->getCalledFunction();
           if (callee && !callee->isDeclaration()) {
             for (unsigned i = 0; i < call->arg_size(); ++i) {
               if (call->getArgOperand(i) == curr) {
                 if (i < callee->arg_size()) {
                    Argument *formalArg = callee->getArg(i);
                    if (m_escaped_values.insert(formalArg).second) {
                      worklist.push_back(formalArg);
                    }
                 }
               }
             }
           }
        } else if (auto *ret = dyn_cast<ReturnInst>(inst)) {
           // Propagate from Return Value -> Call Site
           const Function *F = ret->getFunction();
           for (const User *U : F->users()) {
             if (auto *CB = dyn_cast<CallBase>(U)) {
               if (CB->getCalledFunction() == F) {
                 if (m_escaped_values.insert(CB).second) {
                   worklist.push_back(CB);
                 }
               }
             }
           }
        }
      }
    }
  }
}

} // namespace lotus

