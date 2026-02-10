#ifndef ANALYSIS_MONO_INTRAMONOPROBLEM_H_
#define ANALYSIS_MONO_INTRAMONOPROBLEM_H_

#include "Dataflow/Mono/ControlFlow/IntraCFG.h"
#include "Dataflow/Mono/FlowDirection.h"
#include "Dataflow/Mono/Soundness.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace mono {

struct HasNoConfigurationType {};

template <typename AnalysisDomainTy> class IntraMonoProblem {
public:
  using n_t = typename AnalysisDomainTy::n_t;
  using d_t = typename AnalysisDomainTy::d_t;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using t_t = typename AnalysisDomainTy::t_t;
  using v_t = typename AnalysisDomainTy::v_t;
  using db_t = typename AnalysisDomainTy::db_t;
  using c_t = typename AnalysisDomainTy::c_t;
  using pt_t = typename AnalysisDomainTy::pt_t;

  using ProblemAnalysisDomain = AnalysisDomainTy;

  using ConfigurationTy = HasNoConfigurationType;

  explicit IntraMonoProblem(std::vector<llvm::Function *> EntryPoints = {},
                            pt_t PT = nullptr)
      : PT(PT), EntryPoints(std::move(EntryPoints)) {}

  IntraMonoProblem(const db_t *IRDB, const c_t *CF, pt_t PT,
                   std::vector<std::string> EntryPointNames = {})
      : IRDB(IRDB), CF(CF), PT(std::move(PT)),
        EntryPointNames(std::move(EntryPointNames)) {}

  virtual ~IntraMonoProblem() = default;

  virtual mono_container_t normalFlow(n_t Inst, const mono_container_t &In) = 0;
  virtual mono_container_t merge(const mono_container_t &Lhs,
                                 const mono_container_t &Rhs) = 0;
  virtual bool equal_to(const mono_container_t &Lhs,
                        const mono_container_t &Rhs) = 0;

  virtual mono_container_t allTop() { return mono_container_t{}; }
  virtual std::unordered_map<n_t, mono_container_t> initialSeeds() = 0;
  virtual FlowDirection direction() const { return FlowDirection::Forward; }

  virtual void printContainer(llvm::raw_ostream &, const mono_container_t &) const {
  }

  const std::vector<llvm::Function *> &getEntryPoints() const {
    return EntryPoints;
  }

  const std::vector<std::string> &getEntryPointNames() const {
    return EntryPointNames;
  }

  const db_t *getProjectIRDB() const { return IRDB; }

  const c_t *getCFG() const { return CF; }

  pt_t getPointstoInfo() const { return PT; }
  pt_t getAliasAnalysis() const { return PT; }

  virtual bool setSoundness(Soundness /*S*/) { return false; }

protected:
  const db_t *IRDB = nullptr;
  const c_t *CF = nullptr;
  pt_t PT{};
  std::vector<std::string> EntryPointNames;
  Soundness S = Soundness::Soundy;
  std::vector<llvm::Function *> EntryPoints;
};

} // namespace mono

#endif // ANALYSIS_MONO_INTRAMONOPROBLEM_H_
