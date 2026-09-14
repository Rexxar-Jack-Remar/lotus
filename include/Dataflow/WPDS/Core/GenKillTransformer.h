#ifndef ANALYSIS_DATAFLOW_WPDS_GENKILLTRANSFORMER_H_
#define ANALYSIS_DATAFLOW_WPDS_GENKILLTRANSFORMER_H_

#include "Dataflow/WPDS/Core/GenKillValue.h"

#include <map>
#include <ostream>

namespace wpds {

/**
 * Semiring weight for may-style gen/kill interprocedural dataflow in the WPDS.
 *
 * This weight domain models distributive may analyses over a finite fact
 * domain. `combine()` conservatively joins alternative paths, and `extend()`
 * composes transformers in program order. Supports relational flow:
 *
 *   f(S) = (S \ Kill) U (U_{x in S} Flow(x)) U Gen
 *
 * Corresponds to micro-functions / environment transformers over the fact
 * domain for may analyses. Must analyses are out of scope for this class.
 *
 * @see Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application
 *      to Interprocedural Dataflow Analysis" (Section 4, exploded supergraph)
 */
class GenKillTransformer {
public:
  GenKillTransformer();
  GenKillTransformer(const DataFlowFacts &kill, const DataFlowFacts &gen);
  GenKillTransformer(const DataFlowFacts &kill, const DataFlowFacts &gen,
                     const std::map<Value *, DataFlowFacts> &flow);
  ~GenKillTransformer() = default;

  // Factory method to ensure unique representatives for special values
  static GenKillTransformer *makeGenKillTransformer(const DataFlowFacts &kill,
                                                    const DataFlowFacts &gen);

  static GenKillTransformer *
  makeGenKillTransformer(const DataFlowFacts &kill, const DataFlowFacts &gen,
                         const std::map<Value *, DataFlowFacts> &flow);

  // Semiring operations required by WPDS
  static GenKillTransformer *one();
  static GenKillTransformer *zero();
  // Bottom is kept as a sentinel value for legacy clients. It is not used by
  // the core may-analysis WPDS encoding.
  static GenKillTransformer *bottom();
  GenKillTransformer *extend(GenKillTransformer *other);
  GenKillTransformer *combine(GenKillTransformer *other);
  GenKillTransformer *diff(GenKillTransformer *other);
  GenKillTransformer *quasiOne() const;
  bool equal(GenKillTransformer *other) const;

  // Apply the transformer to a set of facts
  DataFlowFacts apply(const DataFlowFacts &input);

  // Getters for the gen and kill sets
  const DataFlowFacts &getKill() const;
  const DataFlowFacts &getGen() const;
  const std::map<Value *, DataFlowFacts> &getFlow() const;
  const GenKillValue &getValue() const;

  // Debug printing
  std::ostream &print(std::ostream &os) const;

  // Reference counter for ref_ptr
  int count;

private:
  GenKillValue value;

  // Special constructor for one/zero/bottom
  explicit GenKillTransformer(const GenKillValue &value, int);
  explicit GenKillTransformer(const GenKillValue &value);
};

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_GENKILLTRANSFORMER_H_
