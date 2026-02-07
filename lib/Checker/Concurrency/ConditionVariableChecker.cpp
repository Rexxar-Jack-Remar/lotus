/*
 *
 * Author: rainoftime
*/
#include "Checker/Concurrency/ConditionVariableChecker.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

ConditionVariableChecker::ConditionVariableChecker(Module& module,
                                                   ThreadAPI* threadAPI,
                                                   LockSetAnalysis* locksetAnalysis)
    : m_module(module), m_threadAPI(threadAPI), m_locksetAnalysis(locksetAnalysis) {}

std::vector<ConcurrencyBugReport> ConditionVariableChecker::checkConditionVariables() {
    std::vector<ConcurrencyBugReport> reports;

    // Collect (cond, mutex) pairs from all cond_wait calls (signal/broadcast should hold that mutex).
    SmallVector<std::pair<const Value*, const Value*>, 8> condMutexPairs;
    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;
        for (auto& bb : func) {
            for (auto& inst : bb) {
                if (m_threadAPI->isTDCondWait(&inst)) {
                    const Value* cond = m_threadAPI->getCondVal(&inst);
                    const Value* mutex = m_threadAPI->getCondMutex(&inst);
                    if (cond && mutex)
                        condMutexPairs.push_back({cond->stripPointerCasts(), mutex->stripPointerCasts()});
                }
            }
        }
    }

    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;

        for (auto& bb : func) {
            for (auto& inst : bb) {
                // Check signal/broadcast: associated mutex (from cond_wait) should be held.
                if (m_threadAPI->isTDCondSignal(&inst) || m_threadAPI->isTDCondBroadcast(&inst)) {
                    const Value* cond = m_threadAPI->getCondVal(&inst);
                    if (!cond) continue;
                    cond = cond->stripPointerCasts();
                    bool mutexHeld = false;
                    bool anyMatchingCond = false;
                    if (m_locksetAnalysis) {
                        for (const auto& p : condMutexPairs) {
                            if (p.first == cond) {
                                anyMatchingCond = true;
                                if (m_locksetAnalysis->mayHoldLock(&inst, p.second)) {
                                    mutexHeld = true;
                                    break;
                                }
                            }
                        }
                    }
                    // Report when signal/broadcast without holding the mutex associated with this cond (from cond_wait).
                    // Fallback: if no cond_wait in module, report when no lock is held at all.
                    bool noLockHeld = m_locksetAnalysis && m_locksetAnalysis->getMayLockSetAt(&inst).empty();
                    if (!mutexHeld && (anyMatchingCond || (condMutexPairs.empty() && noLockHeld))) {
                        const char* op = m_threadAPI->isTDCondSignal(&inst) ? "signal" : "broadcast";
                        ConcurrencyBugReport report(
                            ConcurrencyBugType::COND_VAR_MISUSE,
                            std::string("Condition variable ") + op + " without holding the associated mutex",
                            BugDescription::BI_HIGH, BugDescription::BC_ERROR);
                        report.addStep(&inst, op);
                        reports.push_back(report);
                    }
                    continue;
                }
                if (m_threadAPI->isTDCondWait(&inst)) {
                    // 1. Check if wait is called with a mutex
                    const Value* mutex = getMutexForCV(&inst);
                    if (mutex)
                        mutex = mutex->stripPointerCasts();
                    if (!mutex) {
                        ConcurrencyBugReport report(
                            ConcurrencyBugType::COND_VAR_MISUSE,
                            "Condition variable wait called without a mutex",
                            BugDescription::BI_HIGH, BugDescription::BC_ERROR
                        );
                        report.addStep(&inst, "Wait called here");
                        reports.push_back(report);
                        continue;
                    }

                    // 2. Check if the mutex is actually held at the point of wait
                    if (m_locksetAnalysis) {
                        LockSet mustHeld = m_locksetAnalysis->getMustLockSetAt(&inst);
                        bool isHeld = false;
                        
                        for (const auto *heldLock : mustHeld) {
                            if (heldLock == mutex) {
                                isHeld = true;
                                break;
                            }
                        }

                        if (!isHeld) {
                            LockSet mayHeld = m_locksetAnalysis->getMayLockSetAt(&inst);
                            bool mayBeHeld = false;
                            for (const auto *heldLock : mayHeld) {
                                if (heldLock == mutex) {
                                    mayBeHeld = true;
                                    break;
                                }
                            }

                            if (!mayBeHeld) {
                                ConcurrencyBugReport report(
                                    ConcurrencyBugType::COND_VAR_MISUSE,
                                    "Mutex not held when calling condition variable wait",
                                    BugDescription::BI_HIGH, BugDescription::BC_ERROR
                                );
                                report.addStep(&inst, "Wait called without holding lock");
                                reports.push_back(report);
                            } else {
                                ConcurrencyBugReport report(
                                    ConcurrencyBugType::COND_VAR_MISUSE,
                                    "Mutex might not be held when calling condition variable wait",
                                    BugDescription::BI_MEDIUM, BugDescription::BC_WARNING
                                );
                                report.addStep(&inst, "Wait called here");
                                reports.push_back(report);
                            }
                        }
                    }
                }
            }
        }
    }
    
    return reports;
}

const Value* ConditionVariableChecker::getMutexForCV(const Instruction* waitInst) const {
    return m_threadAPI->getCondMutex(waitInst);
}

std::string ConditionVariableChecker::getInstructionLocation(const Instruction* inst) const {
    std::string location;
    raw_string_ostream os(location);
    if (const Function* func = inst->getFunction())
        os << func->getName();
    if (const BasicBlock* bb = inst->getParent())
        os << ":" << bb->getName();
    return os.str();
}

} // namespace concurrency
