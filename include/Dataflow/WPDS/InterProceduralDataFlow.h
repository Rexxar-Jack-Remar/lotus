#ifndef ANALYSIS_INTERPROCEDURALDATAFLOW_H_
#define ANALYSIS_INTERPROCEDURALDATAFLOW_H_

#include "Utils/LLVM/SystemHeaders.h"
#include "Dataflow/Mono/DataFlow.h"
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

// DataFlowFacts is the domain of our analysis
class DataFlowFacts {
public:
    DataFlowFacts();
    DataFlowFacts(const std::set<Value*>& facts);
    DataFlowFacts(const DataFlowFacts& other);
    ~DataFlowFacts() = default;

    DataFlowFacts& operator=(const DataFlowFacts& other);
    bool operator==(const DataFlowFacts& other) const;

    // Required set operations for WPDS
    static DataFlowFacts EmptySet();
    static DataFlowFacts UniverseSet();
    static void ClearUniverse();
    static DataFlowFacts Union(const DataFlowFacts& x, const DataFlowFacts& y);
    static DataFlowFacts Intersect(const DataFlowFacts& x, const DataFlowFacts& y);
    static DataFlowFacts Diff(const DataFlowFacts& x, const DataFlowFacts& y);
    static bool Eq(const DataFlowFacts& x, const DataFlowFacts& y);

    // Get the underlying set of facts
    const std::set<Value*>& getFacts() const;
    void addFact(Value* val);
    void removeFact(Value* val);
    bool containsFact(Value* val) const;
    std::size_t size() const;
    bool isEmpty() const;

    // Debug printing
    std::ostream& print(std::ostream& os) const;

private:
    bool is_universe = false;
    std::set<Value*> facts;
};

// GenKillTransformer implements the semiring operations for gen/kill data flow problems
// Extended to support relational flow: f(S) = (S \ Kill) U (U_{x in S \ Kill} Flow(x)) U Gen
class GenKillTransformer {
public:
    GenKillTransformer();
    GenKillTransformer(const DataFlowFacts& kill, const DataFlowFacts& gen);
    GenKillTransformer(const DataFlowFacts& kill, const DataFlowFacts& gen, const std::map<Value*, DataFlowFacts>& flow);
    ~GenKillTransformer() = default;

    // Factory method to ensure unique representatives for special values
    static GenKillTransformer* makeGenKillTransformer(
        const DataFlowFacts& kill, 
        const DataFlowFacts& gen);
        
    static GenKillTransformer* makeGenKillTransformer(
        const DataFlowFacts& kill, 
        const DataFlowFacts& gen,
        const std::map<Value*, DataFlowFacts>& flow);

    // Semiring operations required by WPDS
    static GenKillTransformer* one();
    static GenKillTransformer* zero();
    static GenKillTransformer* bottom();
    GenKillTransformer* extend(GenKillTransformer* other);
    GenKillTransformer* combine(GenKillTransformer* other);
    GenKillTransformer* diff(GenKillTransformer* other);
    GenKillTransformer* quasiOne() const;
    bool equal(GenKillTransformer* other) const;

    // Apply the transformer to a set of facts
    DataFlowFacts apply(const DataFlowFacts& input);

    // Getters for the gen and kill sets
    const DataFlowFacts& getKill() const;
    const DataFlowFacts& getGen() const;
    const std::map<Value*, DataFlowFacts>& getFlow() const;

    // Debug printing
    std::ostream& print(std::ostream& os) const;

    // Reference counter for ref_ptr
    int count;

private:
    DataFlowFacts kill;
    DataFlowFacts gen;
    std::map<Value*, DataFlowFacts> flow; // Map from source fact to generated facts (if source survives kill)
    
    // Special constructor for one/zero/bottom
    GenKillTransformer(const DataFlowFacts& k, const DataFlowFacts& g, const std::map<Value*, DataFlowFacts>& f, int);
};

// InterProceduralDataFlowEngine implements inter-procedural dataflow analysis using WPDS
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

    // Run forward analysis with a caller-provided initial configuration automaton.
    std::unique_ptr<mono::DataFlowResult> runForwardAnalysisWithAutomaton(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const AutomatonBuilder& buildInitialCA);

    // Main method to run backward inter-procedural dataflow analysis
    std::unique_ptr<mono::DataFlowResult> runBackwardAnalysis(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const std::set<Value*>& initialFacts = {});

    // Run backward analysis with a caller-provided initial configuration automaton.
    std::unique_ptr<mono::DataFlowResult> runBackwardAnalysisWithAutomaton(
        Module& m,
        const std::function<GenKillTransformer*(Instruction*)>& createTransformer,
        const AutomatonBuilder& buildInitialCA);

    // Helper methods to query results
    const std::set<Value*>& getInSet(Instruction* inst) const;
    const std::set<Value*>& getOutSet(Instruction* inst) const;

    // Access the last saturated configuration automaton (if any).
    const wpds::CA<GenKillTransformer>* getLastResultAutomaton() const;

    // Query a regular language of stack configurations against the last automaton.
    ::ref_ptr<GenKillTransformer> queryRegularLanguage(
        const wpds::CA<GenKillTransformer>& lang) const;

#ifdef WITNESS_TRACE
    // Return a DOT graph for the witness DAG of a specific transition.
    std::string getWitnessDagDotForTransition(
        wpds::wpds_key_t from,
        wpds::wpds_key_t stack,
        wpds::wpds_key_t to) const;

    // Convenience: return DOT graph for the witness DAG of an instruction query
    // in the default automaton (controlState -> instKey -> accept).
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

    // Map WPDS keys to LLVM values for easy lookup
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
    std::map<Function*, wpds::wpds_key_t> functionToKey;          // function entry keys
    std::map<Function*, wpds::wpds_key_t> functionExitToKey;      // function exit keys
    std::map<Instruction*, wpds::wpds_key_t> instToKey;
    std::map<Instruction*, wpds::wpds_key_t> instPrevKey;         // program point key before inst
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

#endif // ANALYSIS_INTERPROCEDURALDATAFLOW_H_
