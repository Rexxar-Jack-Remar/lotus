#ifndef CHECKER_PULSE_PULSECHECKER_H
#define CHECKER_PULSE_PULSECHECKER_H

#include "Checker/Pulse/PulseDomain.h"
#include "Checker/Pulse/PulseDiagnostic.h"
#include "Checker/Pulse/PulseDisjunctiveDomain.h"
#include "Checker/Pulse/PulseLatentIssue.h"
#include "Checker/Pulse/PulseLoopAbstraction.h"
#include "Checker/Pulse/PulseNonDisjunctiveDomain.h"
#include "Checker/Pulse/PulseOperations.h"
#include "Checker/Pulse/PulseSummary.h"
#include "Checker/Pulse/PulseTransitiveInfo.h"
#include "Checker/Pulse/PulseValueHistory.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <map>
#include <memory>
#include <queue>
#include <tuple>
#include <vector>

namespace llvm {
class BasicBlock;
class Module;
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace pulse {

class PulseModels;

/**
 * PulseChecker: main bug finder using biabductive analysis.
 * Supports CFG-based traversal, GEP/PHI, UnderApproxAA, and interprocedural calls.
 */
class PulseChecker {
private:
    llvm::Module* module_;
    lotus::AliasAnalysisWrapper* aa_;
    AbstractValueFactory factory_;
    PulseOperations ops_;
    std::unique_ptr<PulseModels> models_;
    static constexpr unsigned kMaxDisjuncts = 10u;
    static constexpr unsigned kMaxCallDepth = 5u;

    int useAfterFreeTypeId_;
    int nullDerefTypeId_;
    int uninitializedReadTypeId_;
    int unnecessaryCopyTypeId_;
    int constRefableParamTypeId_;
    int taintErrorTypeId_;

    std::map<const llvm::Function*, std::vector<ExecutionDomain>> function_states_;
    NonDisjunctiveDomain analysis_non_disj_;
    SummaryManager summary_manager_;
    
    // Disjunctive analysis and loop abstraction (optional, can be nullptr)
    std::map<const llvm::Function*, DisjunctiveDomain> disjunctive_domains_;
    std::map<const llvm::Function*, LoopAbstraction> loop_abstractions_;
    
    // Transitive information tracking
    std::map<const llvm::Function*, TransitiveInfo> transitive_info_;
    
    // Latent issues tracking
    std::vector<LatentIssue> latent_issues_;

public:
    explicit PulseChecker(llvm::Module* M, lotus::AliasAnalysisWrapper* AA = nullptr);
    ~PulseChecker();

    void analyze();
    void analyzeFunction(const llvm::Function* F);

    std::vector<ExecutionDomain> executeInstruction(
        const llvm::Instruction* I,
        ExecutionDomain exec_state,
        const llvm::BasicBlock* pred,
        unsigned call_depth);

    ExecutionDomain handleLoad(const llvm::LoadInst* LI,
                               ExecutionDomain exec_state,
                               const llvm::BasicBlock* pred);
    ExecutionDomain handleStore(const llvm::StoreInst* SI,
                                ExecutionDomain exec_state,
                                const llvm::BasicBlock* pred);
    std::vector<ExecutionDomain> handleCall(const llvm::CallInst* CI,
                                            ExecutionDomain exec_state,
                                            const llvm::BasicBlock* pred,
                                            unsigned call_depth);
    ExecutionDomain handleAlloca(const llvm::AllocaInst* AI,
                                 ExecutionDomain exec_state);
    ExecutionDomain handleReturn(const llvm::ReturnInst* RI,
                                 ExecutionDomain exec_state);

    void reportBug(OperationResult kind,
                   const llvm::Instruction* loc,
                   AbstractValue addr,
                   const Trace& trace,
                   const AbductiveDomain* astate = nullptr);

    // Rich diagnostic reporting (for models)
    void reportDiagnostic(const llvm::Instruction* loc, 
                          const std::string& message, 
                          const std::string& type, 
                          int confidence);

    // Overload for structured diagnostics
    void reportDiagnostic(const Diagnostic& diagnostic);

    AbstractValueFactory& getFactory() { return factory_; }
    PulseOperations& getOperations() { return ops_; }

    void registerBugTypes();
    ExecutionDomain initializeFunction(const llvm::Function* F);

    /** Run callee from caller state at CI; returns (exit_state, return AbstractValue) per return. */
    std::vector<std::pair<ExecutionDomain, llvm::Optional<AbstractValue>>>
    runCallee(const llvm::Function* callee,
              const ExecutionDomain& caller_state,
              const llvm::CallInst* CI,
              const llvm::BasicBlock* pred,
              unsigned call_depth);
    
    /**
     * Create summary from function exit states
     */
    void createSummary(const llvm::Function* F,
                       const std::vector<ExecutionDomain>& exit_states,
                       const std::vector<ExecutionDomain>& latent_exit_states);
    
    /**
     * Apply summary at call site
     */
    std::vector<ExecutionDomain> applySummary(
        const llvm::Function* callee,
        const ExecutionDomain& caller_state,
        const llvm::CallInst* CI,
        const llvm::BasicBlock* pred);
    
    /**
     * Improved summary application with materialization
     */
    std::vector<ExecutionDomain> applySummaryImproved(
        const llvm::Function* callee,
        const ExecutionDomain& caller_state,
        const llvm::CallInst* CI,
        const llvm::BasicBlock* pred);
    
    /**
     * Handle comparison instructions (ICmp, FCmp) to update path conditions
     */
    ExecutionDomain handleComparison(const llvm::Instruction* I,
                                     ExecutionDomain exec_state,
                                     const llvm::BasicBlock* pred);
    
    /**
     * Handle library function calls (malloc, free, etc.)
     */
    std::vector<ExecutionDomain> handleLibraryCall(
        const llvm::CallInst* CI,
        ExecutionDomain exec_state,
        const llvm::BasicBlock* pred);

    /**
     * Apply branch condition for a given successor (then/else).
     * Forks state: then = condition true, else = condition false.
     * Returns the resulting state, or None if we don't fork (e.g. can't parse).
     */
    llvm::Optional<ExecutionDomain> applyBranchCondition(
        ExecutionDomain state,
        const llvm::BranchInst* BI,
        unsigned successor_index,
        const llvm::BasicBlock* pred_bb);

    void reportUnnecessaryCopies(const llvm::Function* F);
    void reportConstRefableParams(const llvm::Function* F);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSECHECKER_H
