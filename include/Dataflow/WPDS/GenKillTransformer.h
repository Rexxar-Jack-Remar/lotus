#ifndef ANALYSIS_DATAFLOW_WPDS_GENKILLTRANSFORMER_H_
#define ANALYSIS_DATAFLOW_WPDS_GENKILLTRANSFORMER_H_

#include "Dataflow/WPDS/DataFlowFacts.h"
#include <map>
#include <ostream>

namespace wpds {

/**
 * Semiring weight for gen/kill-style interprocedural dataflow in the WPDS.
 *
 * Implements the combine (meet) and extend (composition) operations required
 * by the weighted PDS. Supports relational flow: f(S) = (S \ Kill) U
 * (U_{x in S \ Kill} Flow(x)) U Gen. Corresponds to the paper's micro-functions
 * / environment transformers over the fact domain.
 *
 * @see Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application
 *      to Interprocedural Dataflow Analysis" (Section 4, exploded supergraph)
 */
class GenKillTransformer {
public:
    GenKillTransformer();
    GenKillTransformer(const DataFlowFacts& kill, const DataFlowFacts& gen);
    GenKillTransformer(const DataFlowFacts& kill, const DataFlowFacts& gen,
                        const std::map<Value*, DataFlowFacts>& flow);
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
    std::map<Value*, DataFlowFacts> flow;  // source fact -> generated facts

    // Special constructor for one/zero/bottom
    GenKillTransformer(const DataFlowFacts& k, const DataFlowFacts& g,
                       const std::map<Value*, DataFlowFacts>& f, int);
};

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_GENKILLTRANSFORMER_H_
