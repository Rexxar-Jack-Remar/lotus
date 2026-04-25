#include "Dataflow/APA/Analyses/LLVM/Inter/Reachability.h"

#include "Dataflow/APA/Adapters/LLVM/InterProblem.h"

namespace elimination {
namespace {

struct InterReachableDomain {
  using n_t = llvm::Instruction *;
  using fact_t = ReachableFact;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
};

class InterElimReachableProblem
    : public LLVMInterEliminationProblem<InterReachableDomain> {
public:
  explicit InterElimReachableProblem(
      llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF)
      : LLVMInterEliminationProblem<InterReachableDomain>(
            std::vector<llvm::Function *>{Entry}, ICF) {}

  fact_t normalFlow(n_t /*Inst*/, const fact_t &In) override { return In; }

  fact_t merge(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs || Rhs;
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t allTop() const override { return false; }

  fact_t callFlow(n_t /*CallSite*/, f_t /*Callee*/, const fact_t &In) override {
    return In;
  }

  fact_t returnFlow(n_t /*CallSite*/, f_t /*Callee*/, n_t /*ExitStmt*/,
                    n_t /*RetSite*/, const fact_t &In) override {
    return In;
  }

  fact_t callToRetFlow(n_t /*CallSite*/, n_t /*RetSite*/,
                       const std::vector<f_t> & /*Callees*/,
                       const fact_t &In) override {
    return In;
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry == nullptr || Entry->empty()) {
      return Seeds;
    }
    Seeds[&*Entry->getEntryBlock().begin()] = true;
    return Seeds;
  }
};

} // namespace

InterReachableResult runInterElimReachable(
    llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF) {
  InterReachableResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimReachableProblem Problem(Entry, ICF);
  InterEliminationSolver<
      InterReachableDomain, kDefaultInterElimReachabilityCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  return Out;
}

} // namespace elimination
