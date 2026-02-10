#ifndef ANALYSIS_MONO_INTERMONOPROBLEM_H_
#define ANALYSIS_MONO_INTERMONOPROBLEM_H_

#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/Mono/IntraMonoProblem.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Instructions.h"

#include <vector>

namespace mono {

template <typename AnalysisDomainTy>
class InterMonoProblem : public IntraMonoProblem<AnalysisDomainTy> {
public:
  using n_t = typename AnalysisDomainTy::n_t;
  using d_t = typename AnalysisDomainTy::d_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;
  using db_t = typename AnalysisDomainTy::db_t;
  using i_t = typename AnalysisDomainTy::i_t;
  using pt_t = typename AnalysisDomainTy::pt_t;

  explicit InterMonoProblem(std::vector<llvm::Function *> EntryPoints = {},
                            pt_t PT = nullptr)
      : IntraMonoProblem<AnalysisDomainTy>(std::move(EntryPoints), PT) {}

  InterMonoProblem(const db_t *IRDB, const i_t *ICF, pt_t PT,
                   std::vector<std::string> EntryPointNames = {})
      : IntraMonoProblem<AnalysisDomainTy>(IRDB, ICF, std::move(PT),
                                           std::move(EntryPointNames)),
        ICF(ICF) {}

  virtual mono_container_t callFlow(n_t CallSite, f_t Callee,
                                    const mono_container_t &In) = 0;
  virtual mono_container_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt,
                                      n_t RetSite,
                                      const mono_container_t &In) = 0;
  virtual mono_container_t callToRetFlow(n_t CallSite, n_t RetSite,
                                        llvm::ArrayRef<f_t> Callees,
                                        const mono_container_t &In) = 0;

  /// Override to provide a more precise call graph (e.g., indirect calls).
  /// Default: direct callee only.
  virtual std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const {
    std::vector<f_t> Callees;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr) {
      return Callees;
    }
    if (auto *Callee = Call->getCalledFunction()) {
      Callees.push_back(Callee);
    }
    return Callees;
  }

  const i_t *getICFG() const { return ICF; }

protected:
  const i_t *ICF = nullptr;
};

} // namespace mono

#endif // ANALYSIS_MONO_INTERMONOPROBLEM_H_
