#ifndef DATAFLOW_APA_CORE_INTERRESULT_H_
#define DATAFLOW_APA_CORE_INTERRESULT_H_

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <vector>

namespace elimination {

struct InterSummarySolveDiagnostics final {
  std::size_t discovered_context_node_count = 0;
  std::size_t seed_count = 0;
  std::size_t equation_node_count = 0;
  std::size_t equation_edge_count = 0;
  std::size_t scc_count = 0;
  std::size_t cyclic_scc_count = 0;
  // EAN/Greedy post-pass instrumentation (see ForwardInterSummarySolver). All
  // zero/empty unless the summary solver ran.
  //   gen_time_us    – build the equation graph + solve it into path exprs,
  //   norm_time_us   – EAN/Greedy optimization of the summary batch (0 if off),
  //   interp_time_us – evaluate summaries into client facts.
  // summary_before/after are the FULL structural stats of the summary batch
  // pre/post optimization (equal when no pass ran). Within a single EAN/Greedy
  // run, summary_before is the raw (Default-configuration) batch and
  // summary_after is the optimized one, so one run yields the Default↔EAN
  // reduction directly. These fill the paper's interprocedural Table VI/VII.
  std::size_t gen_time_us = 0;
  std::size_t norm_time_us = 0;
  std::size_t interp_time_us = 0;
  std::uint64_t semantic_star_time_ns = 0;
  std::size_t star_iterations_total = 0;
  OrderingDiagnostics ordering;
  ean::DagStats summary_before;
  ean::DagStats summary_after;
};

template <unsigned K, typename NodeT> struct ProcedureContextDiagnostics final {
  NodeT Boundary{};
  mono::CallStringCTX<NodeT, K> Context;
  std::vector<NodeT>
      Nodes; // Local node index -> program point for pivot traces.
  SolveDiagnostics Diagnostics;
};

template <unsigned K, typename FactT, typename TransferT,
          typename NodeT = llvm::Instruction *>
class InterDataFlowResultT
    : public dataflow::ContextSensitiveDataFlowResult<K, FactT, NodeT> {
public:
  using Base = dataflow::ContextSensitiveDataFlowResult<K, FactT, NodeT>;
  using fact_t = FactT;
  using transfer_t = TransferT;
  using n_t = NodeT;
  using Context = typename Base::Context;
  using ContextKey = typename Base::ContextKey;

  InterDataFlowResultT() = default;
  explicit InterDataFlowResultT(const Base &Other) : Base(Other) {}

  const fact_t *tryIN(const ContextKey &Key) const {
    auto It = this->getINMap().find(Key);
    if (It == this->getINMap().end()) {
      return nullptr;
    }
    return &It->second;
  }

  const fact_t *tryOUT(const ContextKey &Key) const {
    auto It = this->getOUTMap().find(Key);
    if (It == this->getOUTMap().end()) {
      return nullptr;
    }
    return &It->second;
  }

  const fact_t *tryIN(n_t Inst, const Context &Ctx) const {
    return tryIN(ContextKey{Inst, Ctx});
  }

  const fact_t *tryOUT(n_t Inst, const Context &Ctx) const {
    return tryOUT(ContextKey{Inst, Ctx});
  }

  bool containsInstruction(n_t Inst) const {
    for (const auto &Entry : this->getINMap()) {
      if (Entry.first.Inst == Inst) {
        return true;
      }
    }
    return false;
  }

  std::vector<ContextKey> contextsForInstruction(n_t Inst) const {
    std::vector<ContextKey> Keys;
    for (const auto &Entry : this->getINMap()) {
      if (Entry.first.Inst == Inst) {
        Keys.push_back(Entry.first);
      }
    }
    return Keys;
  }

  void setSolveStatus(SolveStatus S) {
    HasSolveMetadata = true;
    Status = S;
  }

  bool hasSolveMetadata() const { return HasSolveMetadata; }
  SolveStatus solveStatus() const { return Status; }

  void setSummarySolveDiagnostics(const InterSummarySolveDiagnostics &D) {
    HasSummaryDiagnostics = true;
    SummaryDiagnostics = D;
  }

  bool hasSummarySolveDiagnostics() const { return HasSummaryDiagnostics; }
  const InterSummarySolveDiagnostics &summarySolveDiagnostics() const {
    return SummaryDiagnostics;
  }

  void setContextSolveDiagnostics(const SolveDiagnostics &D) {
    HasContextDiagnostics = true;
    ContextDiagnostics = D;
  }
  bool hasContextSolveDiagnostics() const { return HasContextDiagnostics; }
  const SolveDiagnostics &contextSolveDiagnostics() const {
    return ContextDiagnostics;
  }
  void setProcedureContextDiagnostics(
      std::vector<ProcedureContextDiagnostics<K, n_t>> Records) {
    ProcedureDiagnostics = std::move(Records);
  }
  const auto &procedureContextDiagnostics() const {
    return ProcedureDiagnostics;
  }

private:
  bool HasSolveMetadata = false;
  SolveStatus Status = SolveStatus::Ok;
  bool HasSummaryDiagnostics = false;
  InterSummarySolveDiagnostics SummaryDiagnostics;
  bool HasContextDiagnostics = false;
  SolveDiagnostics ContextDiagnostics;
  std::vector<ProcedureContextDiagnostics<K, n_t>> ProcedureDiagnostics;
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_INTERRESULT_H_
