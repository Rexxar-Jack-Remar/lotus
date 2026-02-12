#ifndef ANALYSIS_DATAFLOW_WPDS_INTERPROCEDURALDATAFLOWENGINE_H_
#define ANALYSIS_DATAFLOW_WPDS_INTERPROCEDURALDATAFLOWENGINE_H_

#include "Dataflow/Mono/DataFlow.h"
#include "Dataflow/WPDS/GenKillTransformer.h"
#include "Solvers/WPDS/CA.h"
#include "Solvers/WPDS/WPDS.h"
#include "Solvers/WPDS/key_source.h"
#include "Solvers/WPDS/keys.h"
#include "Solvers/WPDS/ref_ptr.h"
#include "Solvers/WPDS/semiring.h"
#include "llvm/ADT/Optional.h"
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace wpds {

/**
 * Engine for interprocedural dataflow analysis via weighted PDS (WPDS).
 *
 * Encodes a program's supergraph as a WPDS (Section 4 of the paper), runs
 * backward or forward saturation (GPR / Algorithm 1), and extracts IN/OUT
 * facts per instruction. Supports querying with respect to a regular language
 * of stack configurations.
 *
 * @see Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application
 *      to Interprocedural Dataflow Analysis"
 */
class InterProceduralDataFlowEngine {
public:
    using AutomatonBuilder = std::function<void(wpds::CA<GenKillTransformer>&)>;

    InterProceduralDataFlowEngine();
    ~InterProceduralDataFlowEngine() = default;

    // Main method to run forward inter-procedural dataflow analysis
    std::unique_ptr<mono::DataFlowResult> runForwardAnalysis(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const std::set<Value*>& initialFacts = {});

    // Run forward analysis with a caller-provided initial configuration automaton
    std::unique_ptr<mono::DataFlowResult> runForwardAnalysisWithAutomaton(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const AutomatonBuilder& buildInitialCA);

    // Main method to run backward inter-procedural dataflow analysis
    std::unique_ptr<mono::DataFlowResult> runBackwardAnalysis(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const std::set<Value*>& initialFacts = {});

    // Run backward analysis with a caller-provided initial configuration automaton
    std::unique_ptr<mono::DataFlowResult> runBackwardAnalysisWithAutomaton(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const AutomatonBuilder& buildInitialCA);

    // Helper methods to query results
    const std::set<Value*>& getInSet(Instruction* inst) const;
    const std::set<Value*>& getOutSet(Instruction* inst) const;

    // Access the last saturated configuration automaton (if any)
    const wpds::CA<GenKillTransformer>* getLastResultAutomaton() const;

    // Query a regular language of stack configurations against the last automaton
    ::ref_ptr<GenKillTransformer> queryRegularLanguage(
        const wpds::CA<GenKillTransformer>& lang) const;

#ifdef WITNESS_TRACE
    // Return a DOT graph for the witness DAG of a specific transition
    std::string getWitnessDagDotForTransition(
        wpds::wpds_key_t from,
        wpds::wpds_key_t stack,
        wpds::wpds_key_t to) const;

    // Convenience: return DOT graph for the witness DAG of an instruction query
    // in the default automaton (controlState -> instKey -> accept)
    std::string getWitnessDagDotForInstruction(Instruction* inst) const;
#endif

private:
    std::unique_ptr<mono::DataFlowResult> runAnalysisWithAutomaton(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const AutomatonBuilder& buildInitialCA,
        bool isForward);

    // Convert LLVM Module to WPDS
    void buildWPDS(
        Module& m,
        wpds::WPDS<GenKillTransformer>& wpds,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer);

    // Create a configuration automaton for the initial states
    void buildInitialAutomaton(
        Module& m,
        wpds::CA<GenKillTransformer>& ca,
        const std::set<Value*>& initialFacts,
        bool isForward);

    // Map program elements to WPDS keys. getKeyForFunction/Instruction/BasicBlock
    // return the same keys used internally by buildWPDS. getKeyForCallSite and
    // getKeyForReturnSite use a name-based convention and may not match the
    // keys used in the engine's WPDS (which use function-qualified tags).
    wpds::wpds_key_t getKeyForFunction(Function* f);
    wpds::wpds_key_t getKeyForInstruction(Instruction* inst);
    wpds::wpds_key_t getKeyForBasicBlock(BasicBlock* bb);
    wpds::wpds_key_t getKeyForCallSite(CallBase* callInst);
    wpds::wpds_key_t getKeyForReturnSite(CallBase* callInst);

    // Extract dataflow results from the resulting automaton
    void extractResults(
        Module& m,
        wpds::CA<GenKillTransformer>& resultCA,
        std::unique_ptr<mono::DataFlowResult>& result,
        bool isForward);

    // Map program elements to WPDS keys and vice versa
    std::map<Function*, wpds::wpds_key_t> functionToKey;
    std::map<Function*, wpds::wpds_key_t> functionExitToKey;
    std::map<Instruction*, wpds::wpds_key_t> instToKey;
    std::map<Instruction*, wpds::wpds_key_t> instPrevKey;
    std::map<BasicBlock*, wpds::wpds_key_t> bbToKey;
    std::map<wpds::wpds_key_t, Instruction*> keyToInst;

    // Maintain the dataflow result for the most recent analysis
    mutable std::unique_ptr<mono::DataFlowResult> currentResult;
    std::unique_ptr<wpds::CA<GenKillTransformer>> lastResultCA;
    Query lastQuery = Query::user();
    llvm::Optional<wpds::wpds_key_t> lastAcceptState;

    // Single WPDS control state shared by rules and the initial automaton
    wpds::wpds_key_t controlState;
};

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_INTERPROCEDURALDATAFLOWENGINE_H_
