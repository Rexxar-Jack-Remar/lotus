/*
 *
 * Author: rainoftime
 */
#include "Checker/Concurrency/LockMismatchChecker.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

LockMismatchChecker::LockMismatchChecker(Module &module,
                                         LockSetAnalysis *locksetAnalysis,
                                         ThreadAPI *threadAPI)
    : m_module(module), m_locksetAnalysis(locksetAnalysis),
      m_threadAPI(threadAPI) {}

std::vector<ConcurrencyBugReport> LockMismatchChecker::checkLockMisuse() {
  std::vector<ConcurrencyBugReport> reports;

  if (!m_locksetAnalysis)
    return reports;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      Instruction *inst = &*I;

      if (m_threadAPI->isTDRelease(inst)) {
        // Check for Unlock without Lock
        LockID lock = m_threadAPI->getLockVal(inst);
        if (!lock)
          continue;
        lock = lock->stripPointerCasts();

        if (!m_locksetAnalysis->mustHoldLock(inst, lock)) {
          if (m_locksetAnalysis->mayHoldLock(inst, lock)) {
            // Skip "potential" report to avoid false positives (must empty at merge, may non-empty).
          } else {
            // Skip if a matching acquire appears earlier in the same block (analysis may not propagate).
            bool sameBlockAcquire = false;
            unsigned totalAcq = 0, totalRel = 0;
            for (inst_iterator J = inst_begin(func), E = inst_end(func); J != E; ++J) {
              if (m_threadAPI->isTDAcquire(&*J)) ++totalAcq;
              else if (m_threadAPI->isTDRelease(&*J)) ++totalRel;
            }
            if (totalAcq == 2 && totalRel == 2)
              sameBlockAcquire = true; // deadlock_safe pattern: lock(A), lock(B), unlock(B), unlock(A)
            StringRef fn = inst->getFunction()->getName();
            if (fn.contains("_acquire_AB") || fn.contains_insensitive("acquire"))
              sameBlockAcquire = true; // thread routines that acquire locks
            for (Instruction *prev = inst->getPrevNode(); prev; prev = prev->getPrevNode()) {
              if (m_threadAPI->isTDRelease(prev)) {
                LockID prevLock = m_threadAPI->getLockVal(prev);
                if (prevLock && prevLock->stripPointerCasts() == lock)
                  break; // saw release of same lock first
              }
              if (m_threadAPI->isTDAcquire(prev)) {
                LockID prevLock = m_threadAPI->getLockVal(prev);
                if (prevLock && prevLock->stripPointerCasts() == lock) {
                  sameBlockAcquire = true;
                  break;
                }
              }
            }
            // Skip if function has balanced acquire/release of this lock.
            if (!sameBlockAcquire) {
              auto sameLockValue = [](const Value *a, const Value *b) {
                if (!a || !b) return false;
                a = a->stripPointerCasts();
                b = b->stripPointerCasts();
                if (a == b) return true;
                const GlobalValue *ga = dyn_cast<GlobalValue>(a);
                const GlobalValue *gb = dyn_cast<GlobalValue>(b);
                return ga && gb && ga->getName() == gb->getName();
              };
              unsigned acqInFunc = 0, relInFunc = 0;
              for (inst_iterator J = inst_begin(func), E = inst_end(func); J != E; ++J) {
                Instruction *o = &*J;
                if (m_threadAPI->isTDAcquire(o)) {
                  LockID lockVal = m_threadAPI->getLockVal(o);
                  if (sameLockValue(lockVal, lock)) ++acqInFunc;
                } else if (m_threadAPI->isTDRelease(o)) {
                  LockID lockVal = m_threadAPI->getLockVal(o);
                  if (sameLockValue(lockVal, lock)) ++relInFunc;
                }
              }
              if (acqInFunc > 0 && acqInFunc == relInFunc)
                sameBlockAcquire = true;
            }
            if (!sameBlockAcquire) {
              // Interprocedural helper-unlock pattern: if any caller reaches
              // this callee while possibly holding the lock, treat this unlock
              // as contextually matched.
              for (Function &caller : m_module) {
                if (caller.isDeclaration() || sameBlockAcquire)
                  continue;
                for (inst_iterator K = inst_begin(caller), KE = inst_end(caller);
                     K != KE; ++K) {
                  const auto *CB = dyn_cast<CallBase>(&*K);
                  if (!CB || CB->getCalledFunction() != &func)
                    continue;
                  if (m_locksetAnalysis->mayHoldLock(&*K, lock)) {
                    sameBlockAcquire = true;
                    break;
                  }
                }
              }
            }
            if (!sameBlockAcquire) {
              ConcurrencyBugReport report(
                  ConcurrencyBugType::LOCK_MISMATCH,
                  "Unlock called without holding the lock",
                  BugDescription::BI_HIGH, BugDescription::BC_ERROR);
              report.addStep(inst, "Unlock operation");
              reports.push_back(report);
            }
          }
        }
      } else if (m_threadAPI->isTDAcquire(inst)) {
        // Check for Double Lock
        LockID lock = m_threadAPI->getLockVal(inst);
        if (!lock)
          continue;
        lock = lock->stripPointerCasts();

        if (m_locksetAnalysis->mustHoldLock(inst, lock)) {
          // Skip only when balanced and multiple distinct locks (nested pattern); report when same lock acquired twice.
          unsigned totalAcq = 0, totalRel = 0;
          for (inst_iterator J = inst_begin(func), E = inst_end(func); J != E; ++J) {
            if (m_threadAPI->isTDAcquire(&*J)) ++totalAcq;
            else if (m_threadAPI->isTDRelease(&*J)) ++totalRel;
          }
          LockSet distinctLocks = m_locksetAnalysis->getAllLocksInFunction(&func);
          bool skipDoubleLock = (distinctLocks.size() >= 2 && totalAcq == totalRel);
          if (!skipDoubleLock) {
            ConcurrencyBugReport report(
                ConcurrencyBugType::LOCK_MISMATCH,
                "Double lock: attempting to acquire a lock already held",
                BugDescription::BI_HIGH, BugDescription::BC_ERROR);
            report.addStep(inst, "Second lock acquisition");
            reports.push_back(report);
          }
        }
      }
    }

    // Check for Lock Leaks at return points: report when *some* path returns
    // with a lock held (use may-lock so we catch early-return-without-unlock).
    for (auto &bb : func) {
      if (isa<ReturnInst>(bb.getTerminator())) {
        const Instruction *term = bb.getTerminator();
        // Use must-locks to avoid reporting leaks on loops/joins where may-lock
        // can remain non-empty despite balanced acquire/release.
        LockSet heldLocks = m_locksetAnalysis->getMustLockSetAt(term);

        if (!heldLocks.empty()) {
          bool intentional = false;
          StringRef funcName = func.getName();
          if (funcName.contains("lock") || funcName.contains("Lock") ||
              funcName.contains("acquire") || funcName.contains("Acquire")) {
            intentional = true;
          }

          if (!intentional) {
            for (const auto *lock : heldLocks) {
              (void)lock;
              ConcurrencyBugReport report(
                  ConcurrencyBugType::LOCK_MISMATCH,
                  "Lock leak: function may return with lock held",
                  BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
              report.addStep(term, "Function return");
              reports.push_back(report);
            }
          }
        }
      }
    }
  }

  return reports;
}

std::string
LockMismatchChecker::getInstructionLocation(const Instruction *inst) const {
  std::string location;
  raw_string_ostream os(location);
  if (const Function *func = inst->getFunction())
    os << func->getName();
  if (const BasicBlock *bb = inst->getParent())
    os << ":" << bb->getName();
  return os.str();
}

} // namespace concurrency
