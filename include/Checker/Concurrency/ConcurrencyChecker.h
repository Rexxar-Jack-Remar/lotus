#ifndef CONCURRENCY_CHECKER_H
#define CONCURRENCY_CHECKER_H

#include "Analysis/Concurrency/EscapeAnalysis.h"
#include "Analysis/Concurrency/HappensBeforeAnalysis.h"
#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Analysis/Concurrency/MHPAnalysis.h"
#include "Checker/Concurrency/AtomicityChecker.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Checker/Concurrency/ConditionVariableChecker.h"
#include "Checker/Concurrency/DataRaceChecker.h"
#include "Checker/Concurrency/DeadlockChecker.h"
#include "Checker/Concurrency/LockMismatchChecker.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace concurrency {

/**
 * @brief Static checker for concurrency problems including data races, deadlocks, and atomicity violations
 *
 * This checker uses MHP analysis and lock set analysis to detect:
 * - Data races: concurrent accesses to shared variables without proper synchronization
 * - Deadlocks: potential cycles in lock acquisition order
 * - Atomicity violations: operations that should be atomic but may be interleaved
 */
class ConcurrencyChecker {
public:
    explicit ConcurrencyChecker(llvm::Module& module);
    ~ConcurrencyChecker() = default;

    /**
     * @brief Run only the analyses required by the currently enabled checks
     * (Goblint-style config-driven activation). Call after enable*Check() and before runChecks() or dumpAnalysisResults().
     */
    void runAnalyses();

    /**
     * @brief Run all concurrency checks and report bugs to BugReportMgr
     */
    void runChecks();

    /**
     * @brief Check for data races and report to BugReportMgr
     */
    void checkDataRaces();

    /**
     * @brief Check for deadlocks and report to BugReportMgr
     */
    void checkDeadlocks();

    /**
     * @brief Check for atomicity violations and report to BugReportMgr
     */
    void checkAtomicityViolations();

    /**
     * @brief Check for condition variable misuse and report to BugReportMgr
     */
    void checkConditionVariables();

    /**
     * @brief Check for lock mismatches and report to BugReportMgr
     */
    void checkLockMismatches();

    /**
     * @brief Set alias analysis wrapper for better precision
     */
    void setAliasAnalysis(lotus::AliasAnalysisWrapper* aa) {
        m_aliasAnalysis = aa;
        if (m_happensBeforeAnalysis)
            m_happensBeforeAnalysis->setAliasAnalysis(aa);
    }

    /**
     * @brief Enable/disable specific checks
     */
    void enableDataRaceCheck(bool enable) { m_checkDataRaces = enable; }
    void enableDeadlockCheck(bool enable) { m_checkDeadlocks = enable; }
    void enableAtomicityCheck(bool enable) { m_checkAtomicityViolations = enable; }
    void enableCondVarCheck(bool enable) { m_checkCondVars = enable; }
    void enableLockMismatchCheck(bool enable) { m_checkLockMismatches = enable; }

    /**
     * @brief Get statistics about the analysis
     */
    struct Statistics {
        size_t totalInstructions;
        size_t mhpPairs;
        size_t locksAnalyzed;
        size_t dataRacesFound;
        size_t deadlocksFound;
        size_t atomicityViolationsFound;
        size_t condVarBugsFound;
        size_t lockMismatchesFound;
    };

    Statistics getStatistics() const { return m_stats; }

    /**
     * @brief Dump analysis results with source-level debug information
     * 
     * This method dumps the results of fundamental concurrency analyses
     * (MHP, LockSet, Escape) at the source-code level using debug information.
     * Used in analysis-only mode to report facts without performing bug checking.
     * 
     * @param os Output stream to write results to
     * @param jsonFormat If true, output in JSON format; otherwise, human-readable text
     */
    void dumpAnalysisResults(llvm::raw_ostream& os, bool jsonFormat = false) const;

private:
    llvm::Module& m_module;
    std::unique_ptr<mhp::MHPAnalysis> m_mhpAnalysis;
    std::unique_ptr<mhp::LockSetAnalysis> m_locksetAnalysis;
    std::unique_ptr<lotus::EscapeAnalysis> m_escapeAnalysis;
    std::unique_ptr<lotus::HappensBeforeAnalysis> m_happensBeforeAnalysis;
    lotus::AliasAnalysisWrapper* m_aliasAnalysis;
    ThreadAPI* m_threadAPI;

    // Specialized checker components
    std::unique_ptr<DataRaceChecker> m_dataRaceChecker;
    std::unique_ptr<DeadlockChecker> m_deadlockChecker;
    std::unique_ptr<AtomicityChecker> m_atomicityChecker;
    std::unique_ptr<ConditionVariableChecker> m_condVarChecker;
    std::unique_ptr<LockMismatchChecker> m_lockMismatchChecker;

    // Configuration
    bool m_checkDataRaces = true;
    bool m_checkDeadlocks = true;
    bool m_checkAtomicityViolations = true;
    bool m_checkCondVars = true;
    bool m_checkLockMismatches = true;

    // Bug type IDs (registered with BugReportMgr)
    int m_dataRaceTypeId;
    int m_deadlockTypeId;
    int m_atomicityViolationTypeId;
    int m_condVarMisuseTypeId;
    int m_lockMismatchTypeId;

    // Results tracking
    Statistics m_stats;
    
    // Helper to convert ConcurrencyBugReport to BugReport
    void reportBug(const ConcurrencyBugReport& bug_report, int bug_type_id);
};

} // namespace concurrency

#endif // CONCURRENCY_CHECKER_H
