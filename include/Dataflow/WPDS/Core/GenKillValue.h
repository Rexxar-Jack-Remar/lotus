#ifndef ANALYSIS_DATAFLOW_WPDS_GENKILLVALUE_H_
#define ANALYSIS_DATAFLOW_WPDS_GENKILLVALUE_H_

#include "Dataflow/WPDS/Core/DataFlowFacts.h"

#include <map>
#include <ostream>
#include <set>

namespace wpds {

/// Solver-neutral, immutable-by-convention value for the WPDS GEN/KILL
/// semiring. Reference-counting adapters own copies of this value instead of
/// sharing an allocation between the legacy WPDS and WALi ownership schemes.
class GenKillValue {
public:
  enum class Kind { Ordinary, One, Zero, Bottom };

  GenKillValue();
  GenKillValue(const DataFlowFacts &kill, const DataFlowFacts &gen,
               const std::map<Value *, DataFlowFacts> &flow = {});

  static GenKillValue one();
  static GenKillValue zero();
  static GenKillValue bottom();
  static GenKillValue
  normalized(const DataFlowFacts &kill, const DataFlowFacts &gen,
             const std::map<Value *, DataFlowFacts> &flow = {});

  GenKillValue extend(const GenKillValue &other) const;
  GenKillValue combine(const GenKillValue &other) const;
  GenKillValue diff(const GenKillValue &other) const;
  GenKillValue quasiOne() const;
  bool equal(const GenKillValue &other) const;
  bool semanticallyEqual(const GenKillValue &other) const;

  DataFlowFacts apply(const DataFlowFacts &input) const;

  Kind getKind() const;
  const DataFlowFacts &getKill() const;
  const DataFlowFacts &getGen() const;
  const std::map<Value *, DataFlowFacts> &getFlow() const;
  std::ostream &print(std::ostream &os) const;

private:
  GenKillValue(Kind kind, const DataFlowFacts &kill, const DataFlowFacts &gen,
               const std::map<Value *, DataFlowFacts> &flow);

  static std::set<Value *> collectRelevantValues(const GenKillValue &lhs,
                                                 const GenKillValue &rhs);

  Kind kind = Kind::Ordinary;
  DataFlowFacts kill;
  DataFlowFacts gen;
  std::map<Value *, DataFlowFacts> flow;
};

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_GENKILLVALUE_H_
