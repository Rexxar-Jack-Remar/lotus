/** @file Query.h @brief Opaque query handle and guarded query set owned by symbolic state. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_CORE_QUERY_H
#define ANALYSIS_SYMBOLICEXECUTION_CORE_QUERY_H

#include "SymbolicExecution/Core/AnalysisLimit.h"
#include "SymbolicExecution/Core/GuardedValue.h"
#include "SymbolicExecution/Solver/ConstraintRepr.h"

#include <memory>

namespace SymbolicExecution {

class NumericalQuery;
class DirectNumericalQuery;
class IndirectNumericalQuery;

/// Opaque owning handle to a bug query.
///
/// The symbolic state and its summaries store pending bug queries by value, so
/// the handle lives in Core together with the guarded container that holds it.
/// The concrete query hierarchy and all checking policy are defined under
/// Checks; only the shared handle and the container are core state vocabulary.
class NumericalQueryPtr {
  friend class AnalysisSummary;

public:
  NumericalQueryPtr() {}
  NumericalQueryPtr(const std::shared_ptr<NumericalQuery> &V) : Data(V) {}
  NumericalQueryPtr(const std::shared_ptr<DirectNumericalQuery> &V);
  NumericalQueryPtr(const std::shared_ptr<IndirectNumericalQuery> &V);
  NumericalQueryPtr(const NumericalQueryPtr &) = default;
  NumericalQueryPtr &operator=(const NumericalQueryPtr &) = default;

  bool operator==(const NumericalQueryPtr &R) const;

  size_t hash() const;

  const NumericalQuery *operator->() const { return get(); }

  operator bool() const { return Data != nullptr; }

  NumericalQuery *get() const { return Data.get(); }

private:
  std::shared_ptr<NumericalQuery> Data;

  void translate(PathCondSolver *Solver) const;
};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::NumericalQueryPtr> {
  size_t operator()(const SymbolicExecution::NumericalQueryPtr &V) const {
    return V.hash();
  }
};
} // namespace std

namespace SymbolicExecution {

class QuerySet : public GuardedSet<QuerySet, NumericalQueryPtr,
                                   &AnalysisLimit::INST_QUERY_LIMIT_V> {
public:
  QuerySet() = default;
  QuerySet(const NumericalQueryPtr &Q) { addValue(Q); }
};

} // namespace SymbolicExecution

#endif
