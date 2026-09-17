#ifndef DATAFLOW_APA_SOLVER_MODULARINTERSUMMARYSOLVER_H_
#define DATAFLOW_APA_SOLVER_MODULARINTERSUMMARYSOLVER_H_

#include "Dataflow/APA/Core/InterProblem.h"
#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/EAN/EAN.h"
#include "Dataflow/APA/EAN/Greedy.h"
#include "Dataflow/APA/Solver/Inter/CallGraph.h"
#include "Dataflow/APA/Solver/Inter/Transfer.h"
#include "Dataflow/APA/Solver/Equations/Solver.h"
#include "Dataflow/ControlFlow/FlowDirection.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elimination {

// Modular interprocedural summary construction (E6, milestone 3).
//
// For every reachable non-opaque procedure P, build ONE entry->exit
// path-expression summary S_P over P's own CFG, where call sites are turned
// into symbolic `SummaryCall(callsite, callee, retsite)` atoms rather than
// inlining the callee body. This is what makes the construction modular and
// linear in program size: each summary is solved on P's CFG (bounded), never on
// the whole-program x context product that makes the monolithic
// ForwardInterSummarySolver blow up (root causes R1/R2).
//
// Recursion is NOT handled here: a SummaryCall to a same-team procedure is an
// opaque placeholder. Closing recursive teams is an interpretation-time fixpoint
// (milestone 4), guided by the reverse-topological order and `recursive` flag
// carried in the returned CallGraphSCCResult.
//
// Reuse: per-procedure summaries are solved with PathSummaryEquationSolver over
// atom_t (which already handles intra loops via star), NOT the intra
// StateEliminationSolver (that engine is bound to transfer_t atoms and has no
// notion of call sites).
template <typename AnalysisDomainTy> class ModularInterSummaryBuilder final {
public:
  using ProblemTy = InterEliminationProblem<AnalysisDomainTy>;
  using n_t = typename AnalysisDomainTy::n_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using i_t = typename AnalysisDomainTy::i_t;
  using atom_t = InterSummaryTransferAtom<AnalysisDomainTy>;
  using graph_t = PathSummaryEquationGraph<n_t, atom_t>;
  using expr_ref_t = typename graph_t::expr_ref_t;

  struct ProcSummary final {
    // entry->exit summary (union over the procedure's exit points). Null if the
    // procedure has no reachable exit expression.
    expr_ref_t exit;
    // entry->node expression for every instruction, for interpretation (IN).
    std::map<n_t, expr_ref_t> perNode;
  };

  using SummaryTable = std::unordered_map<f_t, ProcSummary>;

  struct Result final {
    SummaryTable summaries;
    CallGraphSCCResult<f_t> callGraph; // reverse-topo order + recursive flags
    // Aggregated per-procedure diagnostics (D4). Structural stats of the union
    // of every procedure's summary batch, before/after per-procedure EAN, plus
    // equation-graph totals and stage timings. summaryBefore == summaryAfter
    // when no post-pass ran, so a single modular run yields the Default<->EAN
    // node reduction directly (mirrors ForwardInterSummarySolver's Table VI/VII).
    ean::DagStats summaryBefore;
    ean::DagStats summaryAfter;
    std::size_t equationNodeCount = 0;
    std::size_t equationEdgeCount = 0;
    std::size_t sccCount = 0;
    std::size_t cyclicSccCount = 0;
    std::size_t genTimeUs = 0;  // build + solve every per-procedure equation graph
    std::size_t normTimeUs = 0; // per-procedure EAN/Greedy optimization (0 if off)
    OrderingDiagnostics ordering;
  };

  ModularInterSummaryBuilder(ProblemTy &Problem, const i_t &ICF,
                             PathSummaryEquationOptions Options = {})
      : Problem(Problem), ICF(ICF), Options(Options) {}

  // Build summaries for every non-opaque procedure reachable from Entries.
  Result build(const std::vector<f_t> &Entries) {
    Result Out;
    BatchBefore.clear();
    BatchAfter.clear();
    CallGraphSCCBuilder<n_t, f_t, i_t> CG(ICF);
    Out.callGraph = CG.build(Entries);
    for (const auto &Comp : Out.callGraph.order) {
      for (const auto &P : Comp.procs) {
        Out.summaries.emplace(P, buildProcedureSummary(P, Out));
      }
    }
    // Aggregate structural stats over the union of all per-procedure batches.
    // computeDagStats walks by pointer identity, so nodes from distinct
    // per-procedure factories are (correctly) counted as distinct.
    Out.summaryBefore = ean::computeDagStats<atom_t>(BatchBefore);
    Out.summaryAfter = ean::computeDagStats<atom_t>(BatchAfter);
    Out.genTimeUs = GenTimeUs;
    Out.normTimeUs = NormTimeUs;
    GenTimeUs = 0;
    NormTimeUs = 0;
    return Out;
  }

private:
  using Fwd = ::dataflow::controlflow::FlowDirection;

  bool isOpaque(const f_t &F) const {
    return ICF.getStartPointsOf(F).empty() || ICF.getExitPointsOf(F).empty();
  }

  // Build P's entry->exit summary over its own CFG. Call sites become symbolic
  // SummaryCall atoms; normal edges become RawNormal atoms; the local
  // call-to-return effect is the bypass edge, mirroring
  // ForwardInterSummarySolver::discoverCallSuccessors but WITHOUT inlining the
  // callee body.
  // Build P's entry->exit summary over its own CFG. Call sites become symbolic
  // SummaryCall atoms; normal edges become RawNormal atoms; the local
  // call-to-return effect is the bypass edge, mirroring
  // ForwardInterSummarySolver::discoverCallSuccessors but WITHOUT inlining the
  // callee body. Diagnostics (equation-graph totals, timings) accumulate into
  // Out; the D4 per-procedure post-pass runs before the summary is returned.
  ProcSummary buildProcedureSummary(const f_t &P, Result &Out) {
    ProcSummary S;
    graph_t Graph;
    auto &E = Graph.exprs();

    const auto Starts = ICF.getStartPointsOf(P);
    if (Starts.empty()) {
      return S;
    }
    // Seed every entry with identity so the forward solve yields entry->node.
    for (const auto &St : Starts) {
      if (St != n_t{}) {
        Graph.setBase(St, E.one());
      }
    }

    for (const auto &U : ICF.getAllInstructionsOf(P)) {
      if (U == n_t{}) {
        continue;
      }
      if (ICF.isCallSite(U)) {
        addCallEdges(Graph, U);
      } else {
        for (const auto &V : ICF.getSuccsOf(U, Fwd::Forward)) {
          if (V == n_t{}) {
            continue;
          }
          Graph.addEdge(U, V, E.atom(atom_t::rawNormal(Problem.edgeTransfer(U, V))));
        }
      }
    }

    auto Opts = Options;
    if (!Opts.Order.IsStarResultCached) {
      Opts.Order.IsStarResultCached = [](const void *) { return false; };
    }
    Opts.Direction = PathSummaryEquationDirection::ForwardPath;
    const auto GenStart = std::chrono::steady_clock::now();
    PathSummaryEquationSolver<n_t, atom_t> Solver(Graph, Opts);
    auto Solved = Solver.solve();
    GenTimeUs += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - GenStart)
            .count());

    const auto &Diag = Solved.diagnostics();
    Out.equationNodeCount += Diag.node_count;
    Out.equationEdgeCount += Diag.edge_count;
    Out.sccCount += Diag.scc_count;
    Out.cyclicSccCount += Diag.cyclic_scc_count;
    Out.ordering.append(Diag.ordering);

    for (const auto &KV : Solved.summaries()) {
      S.perNode.emplace(KV.first, KV.second);
    }
    // S_P = union of entry->exit expressions over all exit points.
    for (const auto &Exit : ICF.getExitPointsOf(P)) {
      auto It = S.perNode.find(Exit);
      if (It == S.perNode.end()) {
        continue;
      }
      S.exit = S.exit ? E.unite(S.exit, It->second) : It->second;
    }

    // D4: optionally optimize THIS procedure's summary batch on its own small
    // graph/factory, then interpretation reads the cheaper equivalent forms.
    applyProcedurePostPass(Graph, S);
    return S;
  }

  // Per-procedure EAN/Greedy post-pass (D4). Optimizes the batch of {every
  // entry->node expression, the entry->exit expression} for one procedure as a
  // single forest so cross-root sharing within the procedure is captured. The
  // SummaryCall/callToRet atoms are opaque leaves to EAN (it never rewrites
  // through them), so callee references survive untouched. No-op unless
  // Options.EAN enables a pass; on any shape mismatch the original batch is kept
  // (root preservation, I3). Every root (pre- and post-pass) is recorded into
  // the builder-wide batches so build() can report the aggregate node reduction.
  void applyProcedurePostPass(graph_t &Graph, ProcSummary &S) {
    std::vector<n_t> Keys;
    std::vector<expr_ref_t> Roots;
    Keys.reserve(S.perNode.size());
    Roots.reserve(S.perNode.size() + 1);
    for (auto &KV : S.perNode) {
      Keys.push_back(KV.first);
      Roots.push_back(KV.second);
    }
    const bool HaveExit = static_cast<bool>(S.exit);
    if (HaveExit) {
      Roots.push_back(S.exit);
    }
    for (const auto &R : Roots) {
      BatchBefore.push_back(R);
    }

    const auto &EAN = Options.EAN;
    if (!EAN.EnableEAN && !EAN.EnableGreedy) {
      for (const auto &R : Roots) {
        BatchAfter.push_back(R); // baseline: after == before
      }
      return;
    }

    const auto NormStart = std::chrono::steady_clock::now();
    std::vector<expr_ref_t> Optimized;
    if (EAN.EnableEAN) {
      ean::ExtractOptions EO = EAN.EANExtract;
      EO.gateMinNodes = EAN.EANMinNodes;
      EO.monotoneGuard = EAN.EANMonotone;
      Optimized = ean::ean<atom_t>(Roots, EAN.EANLaws, EAN.EANCost,
                                   EAN.EANBudget, Graph.exprs(), nullptr, EO);
    } else {
      Optimized = greedySimplify<atom_t>(Roots, Graph.exprs());
    }
    NormTimeUs += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - NormStart)
            .count());

    if (Optimized.size() != Roots.size()) {
      for (const auto &R : Roots) {
        BatchAfter.push_back(R); // I3 fallback: keep the original batch
      }
      return;
    }
    for (std::size_t I = 0; I < Keys.size(); ++I) {
      S.perNode[Keys[I]] = Optimized[I];
    }
    if (HaveExit) {
      S.exit = Optimized.back();
    }
    for (const auto &R : Optimized) {
      BatchAfter.push_back(R);
    }
  }

  // Mirror ForwardInterSummarySolver::discoverCallSuccessors, but replace the
  // callee-body edges with a single symbolic SummaryCall per non-opaque callee.
  void addCallEdges(graph_t &Graph, const n_t &CallSite) {
    auto &E = Graph.exprs();
    const auto Callees = ICF.getCalleesOfCallAt(CallSite);
    for (const auto &RetSite : ICF.getReturnSitesOfCallAt(CallSite)) {
      if (RetSite == n_t{}) {
        continue;
      }
      const auto Transfer = Problem.edgeTransfer(CallSite, RetSite);
      auto Bypass = E.atom(atom_t::rawNormal(Transfer));
      if (Problem.transferSuccessor(Transfer) != n_t{}) {
        Bypass = E.concat(
            Bypass, E.atom(atom_t::callToRet(CallSite, RetSite, Callees)));
      }
      Graph.addEdge(CallSite, RetSite, Bypass);

      for (const auto &Callee : Callees) {
        if (isOpaque(Callee)) {
          continue; // external: only the bypass edge, no summary
        }
        Graph.addEdge(CallSite, RetSite,
                      E.atom(atom_t::summaryCall(CallSite, Callee, RetSite)));
      }
    }
  }

  ProblemTy &Problem;
  const i_t &ICF;
  PathSummaryEquationOptions Options;
  // Union of every procedure's summary roots (entry->node exprs + entry->exit),
  // captured pre- and post- the D4 post-pass, for aggregate DagStats reporting.
  std::vector<expr_ref_t> BatchBefore;
  std::vector<expr_ref_t> BatchAfter;
  std::size_t GenTimeUs = 0;
  std::size_t NormTimeUs = 0;
};

} // namespace elimination

#endif // DATAFLOW_APA_SOLVER_MODULARINTERSUMMARYSOLVER_H_
