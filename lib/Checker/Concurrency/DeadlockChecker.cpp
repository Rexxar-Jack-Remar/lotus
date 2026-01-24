/*
 *
 * Author: rainoftime
*/
#include "Checker/Concurrency/DeadlockChecker.h"

#include <llvm/Support/raw_ostream.h>

//#include <algorithm>
#include <unordered_map>

using namespace llvm;
using namespace mhp;

namespace concurrency {

DeadlockChecker::DeadlockChecker(Module& module,
                               LockSetAnalysis* locksetAnalysis,
                               MHPAnalysis* mhpAnalysis,
                               ThreadAPI* threadAPI)
    : m_module(module), m_locksetAnalysis(locksetAnalysis),
      m_mhpAnalysis(mhpAnalysis), m_threadAPI(threadAPI) {}

std::vector<ConcurrencyBugReport> DeadlockChecker::checkDeadlocks() {
    std::vector<ConcurrencyBugReport> reports;

    auto lockOrderViolations = detectLockOrderViolations();
    auto lostWakeups = detectLostWakeups();
    auto barrierIssues = detectBarrierDivergence();

    for (const auto& violation : lockOrderViolations) {
        mhp::LockID lock1 = violation.first;
        mhp::LockID lock2 = violation.second;

        // Find instructions that acquire these locks
        auto lockAcquires1 = m_locksetAnalysis->getLockAcquires(lock1);
        auto lockAcquires2 = m_locksetAnalysis->getLockAcquires(lock2);
        
        if (lockAcquires1.empty() || lockAcquires2.empty()) continue;

        // Check if threads using these locks can run in parallel
        bool canRunInParallel = false;
        const Instruction* inst1 = nullptr;
        const Instruction* inst2 = nullptr;

        // Check all pairs of acquires to see if any can happen in parallel
        for (const auto* a1 : lockAcquires1) {
            for (const auto* a2 : lockAcquires2) {
                if (m_mhpAnalysis->mayHappenInParallel(a1, a2)) {
                    canRunInParallel = true;
                    inst1 = a1;
                    inst2 = a2;
                    break;
                }
            }
            if (canRunInParallel) break;
        }

        if (!canRunInParallel) continue;

        std::string description = "Potential deadlock: inconsistent lock acquisition order between ";
        description += getLockDescription(lock1);
        description += " and ";
        description += getLockDescription(lock2);
        description += ". Threads acquiring these locks may run in parallel.";

        ConcurrencyBugReport report(
            ConcurrencyBugType::DEADLOCK,
            description,
            BugDescription::BI_HIGH,
            BugDescription::BC_ERROR
        );

        if (inst1) report.addStep(inst1, "Lock 1 acquisition");
        if (inst2) report.addStep(inst2, "Lock 2 acquisition");
        
        reports.push_back(report);
    }

    reports.insert(reports.end(), lostWakeups.begin(), lostWakeups.end());
    reports.insert(reports.end(), barrierIssues.begin(), barrierIssues.end());

    return reports;
}

std::vector<std::pair<mhp::LockID, mhp::LockID>> DeadlockChecker::detectLockOrderViolations() const {
    return m_locksetAnalysis->detectLockOrderInversions();
}

std::string DeadlockChecker::getLockDescription(mhp::LockID lock) const {
    std::string desc;
    raw_string_ostream os(desc);
    if (!lock) {
        os << "<unknown-lock>";
    } else if (lock->hasName()) {
        os << lock->getName();
    } else {
        os << *lock;
    }
    return os.str();
}

bool DeadlockChecker::isLockOperation(const Instruction* inst) const {
    return m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst);
}

mhp::LockID DeadlockChecker::getLockID(const Instruction* inst) const {
    return m_threadAPI->getLockVal(inst);
}

const Instruction* DeadlockChecker::findMatchingUnlock(const Instruction* lockInst) const {
    if (!lockInst) return nullptr;

    mhp::LockID lock = getLockID(lockInst);
    if (!lock) return nullptr;

    auto releases = m_locksetAnalysis->getLockReleases(lock);

    // Find the next release after this acquire
    for (const Instruction* release : releases) {
        if (m_mhpAnalysis->mustPrecede(lockInst, release)) {
            return release;
        }
    }

    return nullptr;
}

std::vector<ConcurrencyBugReport> DeadlockChecker::detectLostWakeups() const {
    std::vector<ConcurrencyBugReport> reports;
    if (!m_threadAPI)
        return reports;

    std::unordered_map<const Value *, std::vector<const Instruction *>> condSignals;
    std::vector<const Instruction *> condWaits;

    auto normalize = [](const Value *v) { return v ? v->stripPointerCasts() : nullptr; };

    for (const Function &func : m_module) {
        if (func.isDeclaration())
            continue;

        for (const auto &bb : func) {
            for (const auto &inst : bb) {
                if (m_threadAPI->isTDCondWait(&inst)) {
                    condWaits.push_back(&inst);
                } else if (m_threadAPI->isTDCondSignal(&inst) ||
                           m_threadAPI->isTDCondBroadcast(&inst)) {
                    const Value *cond = normalize(m_threadAPI->getCondVal(&inst));
                    condSignals[cond].push_back(&inst);
                }
            }
        }
    }

    for (const Instruction *waitInst : condWaits) {
        const Value *cond = normalize(m_threadAPI->getCondVal(waitInst));
        auto it = condSignals.find(cond);
        bool hasMatchingSignal = it != condSignals.end();
        bool hasPotentialWakeup = false;
        const Instruction *exampleSignal = nullptr;

        if (hasMatchingSignal) {
            for (const Instruction *signalInst : it->second) {
                bool canWake = true;
                if (m_mhpAnalysis) {
                    bool parallel = m_mhpAnalysis->mayHappenInParallel(waitInst, signalInst);
                    bool orderedAfter = m_mhpAnalysis->mustPrecede(waitInst, signalInst);
                    canWake = parallel || orderedAfter;
                    if (!canWake && !m_mhpAnalysis->mustPrecede(signalInst, waitInst)) {
                        // If ordering is unknown, still treat as a potential wakeup to avoid false positives.
                        canWake = true;
                    }
                }

                if (canWake) {
                    hasPotentialWakeup = true;
                    exampleSignal = signalInst;
                    break;
                } else if (!exampleSignal) {
                    exampleSignal = signalInst;
                }
            }
        }

        if (!hasMatchingSignal || !hasPotentialWakeup) {
            std::string description =
                "Potential communication deadlock (lost wakeup) on condition variable ";
            description += describeValue(cond);
            description +=
                ": wait may not have a matching signal/broadcast reachable after it.";

            ConcurrencyBugReport report(
                ConcurrencyBugType::DEADLOCK,
                description,
                BugDescription::BI_HIGH,
                BugDescription::BC_ERROR);

            report.addStep(waitInst, "Thread waits on the condition variable here");
            if (exampleSignal) {
                report.addStep(exampleSignal,
                               "Observed signal/broadcast that might not wake this wait");
            }

            reports.push_back(report);
        }
    }

    return reports;
}

std::vector<ConcurrencyBugReport> DeadlockChecker::detectBarrierDivergence() const {
    std::vector<ConcurrencyBugReport> reports;
    if (!m_threadAPI)
        return reports;

    std::unordered_map<const Value *, std::vector<const Instruction *>> barrierWaits;
    auto normalize = [](const Value *v) { return v ? v->stripPointerCasts() : nullptr; };

    for (const Function &func : m_module) {
        if (func.isDeclaration())
            continue;

        for (const auto &bb : func) {
            for (const auto &inst : bb) {
                if (m_threadAPI->isTDBarWait(&inst)) {
                    const Value *barrier = normalize(m_threadAPI->getBarrierVal(&inst));
                    barrierWaits[barrier].push_back(&inst);
                }
            }
        }
    }

    for (const auto &entry : barrierWaits) {
        const Value *barrierVal = entry.first;
        const auto &waits = entry.second;

        if (waits.size() < 2) {
            std::string desc =
                "Potential barrier divergence on barrier " + describeValue(barrierVal) +
                ": only one thread reaches this barrier, so it will block indefinitely.";
            ConcurrencyBugReport report(
                ConcurrencyBugType::DEADLOCK,
                desc,
                BugDescription::BI_HIGH,
                BugDescription::BC_ERROR);
            report.addStep(waits.front(), "Barrier wait with no matching participants");
            reports.push_back(report);
            continue;
        }

        bool hasParallelPair = false;
        const Instruction *partnerA = nullptr;
        const Instruction *partnerB = nullptr;

        for (size_t i = 0; i < waits.size() && !hasParallelPair; ++i) {
            for (size_t j = i + 1; j < waits.size(); ++j) {
                const Instruction *w1 = waits[i];
                const Instruction *w2 = waits[j];
                if (m_mhpAnalysis) {
                    if (m_mhpAnalysis->mayHappenInParallel(w1, w2)) {
                        hasParallelPair = true;
                        partnerA = w1;
                        partnerB = w2;
                        break;
                    }
                } else {
                    // Without MHP info, be conservative and assume they could pair.
                    hasParallelPair = true;
                    partnerA = w1;
                    partnerB = w2;
                    break;
                }
            }
        }

        if (!hasParallelPair) {
            std::string desc =
                "Potential barrier divergence on barrier " + describeValue(barrierVal) +
                ": threads using this barrier do not appear to reach it concurrently.";
            ConcurrencyBugReport report(
                ConcurrencyBugType::DEADLOCK,
                desc,
                BugDescription::BI_HIGH,
                BugDescription::BC_ERROR);
            report.addStep(waits.front(), "Barrier wait that may stall");
            if (waits.size() > 1)
                report.addStep(waits.back(), "Another barrier wait in a different thread");
            reports.push_back(report);
        } else {
            // At least one feasible parallel pair exists, no divergence report needed.
        }
    }

    return reports;
}

bool DeadlockChecker::isSameValue(const Value* lhs, const Value* rhs) const {
    if (!lhs || !rhs) return false;
    return lhs->stripPointerCasts() == rhs->stripPointerCasts();
}

std::string DeadlockChecker::describeValue(const Value* value) const {
    std::string desc;
    raw_string_ostream os(desc);
    if (value) {
        if (value->hasName())
            os << value->getName();
        else
            os << *value;
    } else {
        os << "<unknown>";
    }
    return os.str();
}

} // namespace concurrency
