#pragma once

#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Solver/Inter/Modular/Interpreter.h"
#include "Dataflow/APA/Solver/Inter/Modular/SummaryBuilder.h"

#include <chrono>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace elimination {

// Top-level modular interprocedural driver (E6, milestone 4): builds
// per-procedure summaries, then interprets them into IN/OUT facts with a
// context-insensitive (functional, D1=A) fixpoint over procedure entry facts.
// Results use the empty context key (one IN per instruction).
template <typename AnalysisDomainTy, unsigned K>
class ModularInterSummaryDriver final {
public:
  using ProblemTy = InterEliminationProblem<AnalysisDomainTy>;
  using fact_t = typename AnalysisDomainTy::fact_t;
  using n_t = typename AnalysisDomainTy::n_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using transfer_t = typename AnalysisDomainTy::transfer_t;
  using i_t = typename AnalysisDomainTy::i_t;
  using result_t = InterDataFlowResultT<K, fact_t, transfer_t, n_t>;
  using Context = typename result_t::Context;
  using builder_t = ModularInterSummaryBuilder<AnalysisDomainTy>;
  using interp_t = ModularInterSummaryInterpreter<AnalysisDomainTy, K>;

  ModularInterSummaryDriver(ProblemTy &Problem, const i_t &ICF,
                            PathSummaryEquationOptions Options = {})
      : Problem(Problem), ICF(ICF), Options(Options) {}

  result_t solve(const std::vector<f_t> &Entries, const fact_t &InitialFact) {
    builder_t Builder(Problem, ICF, Options);
    typename builder_t::Result Built;
    try {
      Built = Builder.build(Entries);
    } catch (const std::invalid_argument &) {
      result_t Invalid;
      Invalid.setMissingFactFallback(Problem.bottom());
      Invalid.setSolveStatus(SolveStatus::InvalidProblem);
      return Invalid;
    }
    interp_t Interp(Problem, ICF, Built.summaries);

    const auto InterpStart = std::chrono::steady_clock::now();

    // Merged entry fact per procedure (functional). Entry procedures start at
    // InitialFact; callee entry facts accumulate via callFlow at their call
    // sites, refined to a fixpoint over the reverse-topological order.
    std::unordered_map<f_t, fact_t> EntryFact;
    for (const auto &E : Entries) {
      EntryFact[E] = InitialFact;
    }
    seedEntries(Built, EntryFact, InitialFact);

    const std::size_t Cap = Built.summaries.size() + 2;
    for (std::size_t Pass = 0; Pass < Cap; ++Pass) {
      Interp.beginPass();
      // propagate returns whether any procedure entry fact changed this pass.
      // The interpreter's changedThisPass() only observes the recursion memo
      // table (procApply), NOT the cross-procedure EntryFact flow driven here,
      // so the outer fixpoint must watch BOTH signals. Watching only the
      // interpreter's flag lets the loop break while entry facts are still
      // propagating down a deep call chain (main -> ... -> leaf), which
      // under-approximates reachability for the deepest callees.
      const bool EntryChanged = propagate(Built, Interp, EntryFact);
      if (!EntryChanged && !Interp.changedThisPass() && Pass > 0) {
        break;
      }
    }

    result_t Result;
    Result.setMissingFactFallback(Problem.bottom());
    std::size_t EmittedNodes = 0;
    emit(Built, Interp, EntryFact, Result, EmittedNodes);
    const auto InterpUs = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - InterpStart)
            .count());

    // Surface the same Table VI/VII diagnostics the whole-program summary solver
    // reports, so `--inter-engine=modular` prints [inter-summary]/[dagstats-*] and the
    // Default<->EAN node reduction is directly observable.
    InterSummarySolveDiagnostics Diag;
    Diag.discovered_context_node_count = EmittedNodes;
    Diag.seed_count = Entries.size();
    Diag.equation_node_count = Built.equationNodeCount;
    Diag.equation_edge_count = Built.equationEdgeCount;
    Diag.scc_count = Built.sccCount;
    Diag.cyclic_scc_count = Built.cyclicSccCount;
    Diag.gen_time_us = Built.genTimeUs;
    Diag.norm_time_us = Built.normTimeUs;
    Diag.interp_time_us = InterpUs;
    Diag.semantic_star_time_ns = Interp.semanticStarTimeNs();
    Diag.star_iterations_total = Interp.starIterations();
    Diag.ordering = std::move(Built.ordering);
    Diag.summary_before = Built.summaryBefore;
    Diag.summary_after = Built.summaryAfter;
    Result.setSummarySolveDiagnostics(Diag);

    Result.setSolveStatus(SolveStatus::Ok);
    return Result;
  }

private:
  // Ensure every discovered procedure has an entry-fact slot (default bottom).
  void seedEntries(const typename builder_t::Result &Built,
                   std::unordered_map<f_t, fact_t> &EntryFact,
                   const fact_t &InitialFact) {
    for (const auto &Comp : Built.callGraph.order) {
      for (const auto &P : Comp.procs) {
        EntryFact.emplace(P, Problem.bottom());
      }
    }
    (void)InitialFact;
  }

  // For each procedure (callees first), evaluate call-site reaching facts and
  // push callFlow into callees' entry facts. Returns whether any callee entry
  // fact changed, so the driver's outer fixpoint can detect that entry facts
  // are still propagating even when the interpreter's recursion memo is stable.
  bool propagate(const typename builder_t::Result &Built, interp_t &Interp,
                 std::unordered_map<f_t, fact_t> &EntryFact) {
    bool EntryChanged = false;
    for (const auto &Comp : Built.callGraph.order) {
      for (const auto &P : Comp.procs) {
        auto SumIt = Built.summaries.find(P);
        if (SumIt == Built.summaries.end()) {
          continue;
        }
        const fact_t In = EntryFact[P];
        for (const auto &Inst : ICF.getAllInstructionsOf(P)) {
          if (!ICF.isCallSite(Inst)) {
            continue;
          }
          auto NodeIt = SumIt->second.perNode.find(Inst);
          if (NodeIt == SumIt->second.perNode.end()) {
            continue;
          }
          const fact_t AtCall = Interp.evalExpr(NodeIt->second, In);
          for (const auto &Callee : ICF.getCalleesOfCallAt(Inst)) {
            auto CalleeIt = EntryFact.find(Callee);
            if (CalleeIt == EntryFact.end()) {
              continue; // opaque callee
            }
            const fact_t Contribution =
                Problem.callFlow(Inst, Callee, AtCall);
            const fact_t Merged =
                Problem.join(CalleeIt->second, Contribution);
            if (!Problem.equal(Merged, CalleeIt->second)) {
              CalleeIt->second = Merged;
              EntryChanged = true;
            }
          }
        }
      }
    }
    return EntryChanged;
  }

  void emit(const typename builder_t::Result &Built, interp_t &Interp,
            const std::unordered_map<f_t, fact_t> &EntryFact,
            result_t &Result, std::size_t &EmittedNodes) {
    const Context Empty{};
    for (const auto &Comp : Built.callGraph.order) {
      for (const auto &P : Comp.procs) {
        auto SumIt = Built.summaries.find(P);
        if (SumIt == Built.summaries.end()) {
          continue;
        }
        auto EF = EntryFact.find(P);
        const fact_t In = EF == EntryFact.end() ? Problem.bottom() : EF->second;
        for (const auto &KV : SumIt->second.perNode) {
          const fact_t IN = Interp.evalExpr(KV.second, In);
          Result.IN(KV.first, Empty) = IN;
          Result.OUT(KV.first, Empty) =
              Problem.applyTransfer(Problem.edgeTransfer(KV.first, n_t{}), IN);
          ++EmittedNodes;
        }
      }
    }
  }

  ProblemTy &Problem;
  const i_t &ICF;
  PathSummaryEquationOptions Options;
};

} // namespace elimination

