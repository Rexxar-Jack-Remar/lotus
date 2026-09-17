#ifndef DATAFLOW_APA_SOLVER_INTERSUMMARYTRANSFER_H_
#define DATAFLOW_APA_SOLVER_INTERSUMMARYTRANSFER_H_

#include "Dataflow/APA/Core/InterProblem.h"
#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Core/Timing.h"
#include "Dataflow/ControlFlow/FlowDirection.h"
#include "Dataflow/Mono/Core/CallStringContext.h"

#include <cstddef>
#include <vector>

namespace elimination {

// First-class interprocedural transfer atom for summary-expression solving.
//
// Current APA interprocedural clients expose normalFlow/callFlow/returnFlow
// hooks. This atom type reifies those hooks so a solver can build and evaluate
// path expressions whose labels are interprocedural effects instead of only raw
// CFG edge transfers.
template <typename AnalysisTypesT> struct InterSummaryTransferAtom final {
  using n_t = typename AnalysisTypesT::n_t;
  using f_t = typename AnalysisTypesT::f_t;
  using transfer_t = typename AnalysisTypesT::transfer_t;

  enum class Kind {
    RawNormal,
    Normal,
    CallEntry,
    ReturnExit,
    CallToRet,
    // Symbolic reference to a callee's entry->exit path-expression summary at a
    // call site (E6 modular interprocedural solver). NOT inlined: evaluation
    // means In -> callFlow(CallSite,Callee) -> evaluate S_Callee ->
    // returnFlow(...,RetSite) -> Out, performed by ModularInterSummarySolver's
    // evaluator, which carries the {callee -> summary} table. The whole-program
    // ForwardInterSummarySolver never produces this kind.
    SummaryCall,
  };

  static InterSummaryTransferAtom rawNormal(transfer_t Transfer) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::RawNormal;
    Atom.NormalTransfer = std::move(Transfer);
    return Atom;
  }

  static InterSummaryTransferAtom normal(transfer_t Transfer) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::Normal;
    Atom.NormalTransfer = std::move(Transfer);
    return Atom;
  }

  static InterSummaryTransferAtom callEntry(n_t CallSite, f_t Callee) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::CallEntry;
    Atom.CallSite = CallSite;
    Atom.Callee = Callee;
    return Atom;
  }

  static InterSummaryTransferAtom returnExit(n_t CallSite, f_t Callee,
                                             n_t ExitStmt, n_t RetSite) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::ReturnExit;
    Atom.CallSite = CallSite;
    Atom.Callee = Callee;
    Atom.ExitStmt = ExitStmt;
    Atom.RetSite = RetSite;
    return Atom;
  }

  static InterSummaryTransferAtom callToRet(n_t CallSite, n_t RetSite,
                                            std::vector<f_t> Callees) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::CallToRet;
    Atom.CallSite = CallSite;
    Atom.RetSite = RetSite;
    Atom.Callees = std::move(Callees);
    return Atom;
  }

  // E6: symbolic reference to Callee's entry->exit summary at CallSite, returning
  // to RetSite. Reuses the CallSite/Callee/RetSite fields; evaluated by the
  // modular solver (milestone 3/4), not by InterSummaryTransferEvaluator.
  static InterSummaryTransferAtom summaryCall(n_t CallSite, f_t Callee,
                                              n_t RetSite) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::SummaryCall;
    Atom.CallSite = CallSite;
    Atom.Callee = Callee;
    Atom.RetSite = RetSite;
    return Atom;
  }

  Kind K = Kind::Normal;
  transfer_t NormalTransfer{};
  n_t CallSite{};
  f_t Callee{};
  n_t ExitStmt{};
  n_t RetSite{};
  std::vector<f_t> Callees;

  // Value equality over all discriminant fields. Enables hash-consed dedup of
  // summary atoms in PathExprFactory (without it the factory mints a fresh node
  // per atom(), collapsing all structural sharing of the interprocedural DAG).
  friend bool operator==(const InterSummaryTransferAtom &A,
                         const InterSummaryTransferAtom &B) {
    return A.K == B.K && A.NormalTransfer == B.NormalTransfer &&
           A.CallSite == B.CallSite && A.Callee == B.Callee &&
           A.ExitStmt == B.ExitStmt && A.RetSite == B.RetSite &&
           A.Callees == B.Callees;
  }
  friend bool operator!=(const InterSummaryTransferAtom &A,
                         const InterSummaryTransferAtom &B) {
    return !(A == B);
  }
};

} // namespace elimination

// Hash over all discriminant fields, mirroring operator==. Placed after the
// type is complete so PathExprFactory<InterSummaryTransferAtom>::atom() selects
// its hash-consing path (is_std_hashable becomes true).
namespace std {
template <typename AnalysisDomainTy>
struct hash<elimination::InterSummaryTransferAtom<AnalysisDomainTy>> {
  std::size_t operator()(
      const elimination::InterSummaryTransferAtom<AnalysisDomainTy> &A) const {
    using Atom = elimination::InterSummaryTransferAtom<AnalysisDomainTy>;
    std::size_t H = std::hash<int>{}(static_cast<int>(A.K));
    const auto mix = [&H](std::size_t V) {
      H ^= V + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    };
    mix(std::hash<typename Atom::transfer_t>{}(A.NormalTransfer));
    mix(std::hash<typename Atom::n_t>{}(A.CallSite));
    mix(std::hash<typename Atom::f_t>{}(A.Callee));
    mix(std::hash<typename Atom::n_t>{}(A.ExitStmt));
    mix(std::hash<typename Atom::n_t>{}(A.RetSite));
    for (const auto &C : A.Callees) {
      mix(std::hash<typename Atom::f_t>{}(C));
    }
    return H;
  }
};
} // namespace std

#endif // DATAFLOW_APA_SOLVER_INTERSUMMARYTRANSFER_H_
