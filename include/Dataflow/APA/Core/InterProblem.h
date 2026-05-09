#ifndef DATAFLOW_APA_CORE_INTERPROBLEM_H_
#define DATAFLOW_APA_CORE_INTERPROBLEM_H_

#include "Dataflow/ControlFlow/FlowDirection.h"

#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elimination {

template <typename AnalysisDomainTy> class InterEliminationProblem {
public:
  using n_t = typename AnalysisDomainTy::n_t;
  using fact_t = typename AnalysisDomainTy::fact_t;
  using transfer_t = typename AnalysisDomainTy::transfer_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using i_t = typename AnalysisDomainTy::i_t;

  explicit InterEliminationProblem(std::vector<f_t> EntryPoints = {},
                                   const i_t *ICF = nullptr)
      : EntryPoints(std::move(EntryPoints)), ICF(ICF) {}

  virtual ~InterEliminationProblem() = default;

  virtual fact_t normalFlow(n_t Inst, const fact_t &In) = 0;
  virtual fact_t merge(const fact_t &Lhs, const fact_t &Rhs) const = 0;
  virtual bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const = 0;

  virtual transfer_t edgeTransfer(n_t Src, n_t Dst) const {
    if constexpr (std::is_same_v<transfer_t, n_t>) {
      return direction() == ::dataflow::controlflow::FlowDirection::Backward
                 ? Dst
                 : Src;
    } else {
      (void)Src;
      (void)Dst;
      return transfer_t{};
    }
  }

  virtual fact_t applyTransfer(const transfer_t &T, const fact_t &In) const {
    if constexpr (std::is_same_v<transfer_t, n_t>) {
      return const_cast<InterEliminationProblem *>(this)->normalFlow(T, In);
    } else {
      (void)T;
      return In;
    }
  }

  virtual n_t transferNode(const transfer_t &T) const {
    if constexpr (std::is_same_v<transfer_t, n_t>) {
      return T;
    } else {
      (void)T;
      return n_t{};
    }
  }

  virtual n_t transferSuccessor(const transfer_t &T) const {
    (void)T;
    return n_t{};
  }

  virtual fact_t allTop() const { return fact_t{}; }

  virtual std::unordered_map<n_t, fact_t> initialSeeds() = 0;

  virtual ::dataflow::controlflow::FlowDirection direction() const {
    return ::dataflow::controlflow::FlowDirection::Forward;
  }

  virtual fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) = 0;
  virtual fact_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt, n_t RetSite,
                            const fact_t &In) = 0;
  virtual fact_t returnFlowWithCallerFact(n_t CallSite, f_t Callee,
                                          n_t ExitStmt, n_t RetSite,
                                          const fact_t &CalleeExit,
                                          const fact_t &CallerFact) {
    (void)CallerFact;
    return returnFlow(CallSite, Callee, ExitStmt, RetSite, CalleeExit);
  }
  virtual fact_t callToRetFlow(n_t CallSite, n_t RetSite,
                               const std::vector<f_t> &Callees,
                               const fact_t &In) = 0;

  virtual std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const = 0;

  const std::vector<f_t> &getEntryPoints() const { return EntryPoints; }
  const i_t *getICFG() const { return ICF; }

private:
  std::vector<f_t> EntryPoints;
  const i_t *ICF = nullptr;
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_INTERPROBLEM_H_
