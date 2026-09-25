#pragma once

#include "Dataflow/APA/Core/InterProblem.h"
#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/EAN/EAN.h"
#include "Dataflow/APA/EAN/Greedy.h"
#include "Dataflow/APA/Solver/Inter/Interpreter.h"
#include "Dataflow/APA/Solver/Equations/Solver.h"
#include "Dataflow/ControlFlow/FlowDirection.h"
#include "Dataflow/Mono/Core/CallStringContext.h"

#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace elimination {

// Forward context-sensitive interprocedural solver based on one global graph of
// path-summary equations. Nodes are instruction/context pairs. Edges are
// first-class interprocedural transfer atoms, so calls are represented as:
//
//   callsite --RawNormal.CallEntry--> callee entry
//   callsite --RawNormal.CallToRet--> return site
//   callee exit --RawNormal.ReturnExit--> return site
//
// The resulting equation graph is solved with PathSummaryEquationSolver, which
// closes recursive SCCs with Star expressions and evaluates SCCs in dependency
// order.
template <typename AnalysisTypesT, unsigned K>
class ForwardInterSummarySolver final {
public:
  using ProblemTy = InterEliminationProblem<AnalysisTypesT>;
  using fact_t = typename AnalysisTypesT::fact_t;
  using n_t = typename AnalysisTypesT::n_t;
  using f_t = typename AnalysisTypesT::f_t;
  using transfer_t = typename AnalysisTypesT::transfer_t;
  using i_t = typename AnalysisTypesT::i_t;
  using result_t = InterDataFlowResultT<K, fact_t, transfer_t, n_t>;
  using Context = mono::CallStringCTX<n_t, K>;

  struct ContextKey final {
    n_t Inst{};
    Context Ctx;

    bool operator<(const ContextKey &Other) const {
      if (Inst != Other.Inst) {
        return Inst < Other.Inst;
      }
      return Ctx < Other.Ctx;
    }

    bool operator==(const ContextKey &Other) const {
      return Inst == Other.Inst && Ctx == Other.Ctx;
    }
  };

  struct ContextKeyHash final {
    std::size_t operator()(const ContextKey &Key) const {
      std::size_t H = std::hash<n_t>{}(Key.Inst);
      H ^= std::hash<Context>{}(Key.Ctx) + 0x9e3779b97f4a7c15ULL + (H << 6) +
           (H >> 2);
      return H;
    }
  };

  using atom_t = InterSummaryTransferAtom<AnalysisTypesT>;
  using summary_graph_t = PathSummaryEquationGraph<ContextKey, atom_t>;
  using expr_ref_t = typename summary_graph_t::expr_ref_t;

  struct Diagnostics final {
    PathSummaryEquationDiagnostics equation_graph;
    std::size_t discovered_context_node_count = 0;
    std::size_t seed_count = 0;
    // EAN/Greedy post-pass instrumentation (see applySummaryPostPass). Zero
    // unless the pass ran.
    std::size_t gen_time_us = 0;
    std::size_t norm_time_us = 0;
    std::size_t interp_time_us = 0;
    std::uint64_t semantic_star_time_ns = 0;
    std::size_t star_iterations_total = 0;
    ean::DagStats summary_before;
    ean::DagStats summary_after;
  };

  explicit ForwardInterSummarySolver(ProblemTy &Problem,
                                     PathSummaryEquationOptions Options = {})
      : Problem(Problem), Options(Options) {}

  SolveStatus solve() {
    DiagnosticsValue = {};
    HaveResult = false;
    const auto *ICFPtr = Problem.getICFG();
    if (ICFPtr == nullptr ||
        Problem.direction() != dataflow::controlflow::FlowDirection::Forward) {
      HaveResult = false;
      return LastStatus = SolveStatus::InvalidProblem;
    }
    ICF = ICFPtr;
    SeedFacts = Problem.initialSeeds();

    Graph = summary_graph_t{};
    Discovered.clear();
    InitialFact = Problem.bottom();
    HaveInitialFact = false;

    const auto GenStart = std::chrono::steady_clock::now();
    discoverEquationGraph();
    auto SolverOptions = Options;
    if (!SolverOptions.Order.IsStarResultCached) {
      // This interpreter starts fresh after construction: a known empty cache,
      // unlike an absent metadata provider in the generic equation engine.
      SolverOptions.Order.IsStarResultCached = [](const void *) { return false; };
    }
    SolverOptions.Direction = PathSummaryEquationDirection::ForwardPath;
    PathSummaryEquationSolver<ContextKey, atom_t> Solver(Graph, SolverOptions);
    typename PathSummaryEquationSolver<ContextKey, atom_t>::result_t Summary;
    try {
      Summary = Solver.solve();
    } catch (const std::invalid_argument &) {
      return LastStatus = SolveStatus::InvalidProblem;
    }
    DiagnosticsValue.equation_graph = Summary.diagnostics();
    DiagnosticsValue.gen_time_us += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - GenStart)
            .count());

    // EAN/Greedy post-optimization of the solved summary batch (no-op unless
    // enabled in Options.EAN). Runs before interpretation so the cheaper,
    // semantics-preserving IR is what gets evaluated into client facts.
    applySummaryPostPass(Summary);

    Result = result_t{};
    Result.setMissingFactFallback(Problem.bottom());
    evaluateSummaries(Summary);
    Result.setSolveStatus(SolveStatus::Ok);
    HaveResult = true;
    return LastStatus = SolveStatus::Ok;
  }

  const result_t *getResults() const { return HaveResult ? &Result : nullptr; }
  SolveStatus getLastStatus() const { return LastStatus; }
  const Diagnostics &diagnostics() const { return DiagnosticsValue; }

  InterSummarySolveDiagnostics resultDiagnostics() const {
    InterSummarySolveDiagnostics Out;
    Out.discovered_context_node_count =
        DiagnosticsValue.discovered_context_node_count;
    Out.seed_count = DiagnosticsValue.seed_count;
    Out.equation_node_count = DiagnosticsValue.equation_graph.node_count;
    Out.equation_edge_count = DiagnosticsValue.equation_graph.edge_count;
    Out.scc_count = DiagnosticsValue.equation_graph.scc_count;
    Out.cyclic_scc_count = DiagnosticsValue.equation_graph.cyclic_scc_count;
    Out.gen_time_us = DiagnosticsValue.gen_time_us;
    Out.norm_time_us = DiagnosticsValue.norm_time_us;
    Out.interp_time_us = DiagnosticsValue.interp_time_us;
    Out.semantic_star_time_ns = DiagnosticsValue.semantic_star_time_ns;
    Out.star_iterations_total = DiagnosticsValue.star_iterations_total;
    Out.ordering = DiagnosticsValue.equation_graph.ordering;
    Out.summary_before = DiagnosticsValue.summary_before;
    Out.summary_after = DiagnosticsValue.summary_after;
    return Out;
  }

private:
  expr_ref_t rawNormalExpr(transfer_t Transfer) {
    return Graph.exprs().atom(atom_t::rawNormal(std::move(Transfer)));
  }

  expr_ref_t concat(const expr_ref_t &Lhs, const expr_ref_t &Rhs) {
    return Graph.exprs().concat(Lhs, Rhs);
  }

  void rememberInitialFact(const fact_t &Fact) {
    if (!HaveInitialFact) {
      InitialFact = Fact;
      HaveInitialFact = true;
      return;
    }
    InitialFact = Problem.join(InitialFact, Fact);
  }

  void enqueue(ContextKey Key, std::deque<ContextKey> &Worklist) {
    if (Key.Inst == n_t{}) {
      return;
    }
    if (Discovered.insert(Key).second) {
      Graph.addNode(Key);
      Worklist.push_back(std::move(Key));
    }
  }

  void addSeed(ContextKey Key, const fact_t &Fact,
               std::deque<ContextKey> &Worklist) {
    enqueue(Key, Worklist);
    Graph.setBase(Key, Graph.exprs().one());
    rememberInitialFact(Fact);
    ++DiagnosticsValue.seed_count;
  }

  void addEquationEdge(const ContextKey &Target, const ContextKey &Source,
                       expr_ref_t Weight, std::deque<ContextKey> &Worklist) {
    enqueue(Source, Worklist);
    enqueue(Target, Worklist);
    Graph.addEdge(Source, Target, std::move(Weight));
  }

  void discoverEquationGraph() {
    std::deque<ContextKey> Worklist;
    Context EmptyCtx;

    for (const auto &Seed : SeedFacts) {
      addSeed({Seed.first, EmptyCtx}, Seed.second, Worklist);
    }

    if (SeedFacts.empty()) {
      for (auto Entry : Problem.getEntryPoints()) {
        if (Entry == f_t{}) {
          continue;
        }
        auto Starts = ICF->getStartPointsOf(Entry);
        if (!Starts.empty() && Starts.front() != n_t{}) {
          addSeed({Starts.front(), EmptyCtx}, Problem.bottom(), Worklist);
        }
      }
    }

    while (!Worklist.empty()) {
      auto Key = Worklist.front();
      Worklist.pop_front();
      discoverSuccessors(Key, Worklist);
    }

    DiagnosticsValue.discovered_context_node_count = Discovered.size();
  }

  void discoverSuccessors(const ContextKey &Key,
                          std::deque<ContextKey> &Worklist) {
    auto Inst = Key.Inst;
    if (Inst == n_t{}) {
      return;
    }

    if (ICF->isCallSite(Inst)) {
      discoverCallSuccessors(Key, Worklist);
      return;
    }

    for (auto Succ :
         ICF->getSuccsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
      if (Succ == n_t{}) {
        continue;
      }
      auto Weight = rawNormalExpr(Problem.edgeTransfer(Inst, Succ));
      addEquationEdge({Succ, Key.Ctx}, Key, Weight, Worklist);
    }
  }

  void discoverCallSuccessors(const ContextKey &Key,
                              std::deque<ContextKey> &Worklist) {
    const auto CallSite = Key.Inst;
    const auto Callees = ICF->getCalleesOfCallAt(CallSite);

    for (auto RetSite : ICF->getReturnSitesOfCallAt(CallSite)) {
      if (RetSite == n_t{}) {
        continue;
      }
      auto Transfer = Problem.edgeTransfer(CallSite, RetSite);
      auto Bypass = rawNormalExpr(Transfer);
      if (Problem.transferSuccessor(Transfer) != n_t{}) {
        auto CallToRet =
            Graph.exprs().atom(atom_t::callToRet(CallSite, RetSite, Callees));
        Bypass = concat(Bypass, CallToRet);
      }
      addEquationEdge({RetSite, Key.Ctx}, Key, Bypass, Worklist);
    }

    Context CalleeCtx = Key.Ctx;
    CalleeCtx.push_back(CallSite);
    for (auto Callee : Callees) {
      auto Starts = ICF->getStartPointsOf(Callee);
      auto Exits = ICF->getExitPointsOf(Callee);
      if (Starts.empty() || Exits.empty()) {
        continue;
      }

      auto Enter = Graph.exprs().atom(atom_t::callEntry(CallSite, Callee));
      auto RawCall = rawNormalExpr(Problem.edgeTransfer(CallSite, n_t{}));
      addEquationEdge({Starts.front(), CalleeCtx}, Key, concat(RawCall, Enter),
                      Worklist);

      for (auto Exit : Exits) {
        if (Exit == n_t{}) {
          continue;
        }
        ContextKey ExitKey{Exit, CalleeCtx};
        enqueue(ExitKey, Worklist);
        auto RawExit = rawNormalExpr(Problem.edgeTransfer(Exit, n_t{}));
        for (auto RetSite : ICF->getReturnSitesOfCallAt(CallSite)) {
          if (RetSite == n_t{}) {
            continue;
          }
          auto Return = Graph.exprs().atom(
              atom_t::returnExit(CallSite, Callee, Exit, RetSite));
          addEquationEdge({RetSite, Key.Ctx}, ExitKey, concat(RawExit, Return),
                          Worklist);
        }
      }
    }
  }

  // EAN/Greedy post-optimization: replace each context summary's path
  // expression with a reuse-aware, cost-minimized equivalent (as one batch),
  // then interpretation reads the optimized forms. No-op unless Options.EAN
  // enables a pass. Semantics are preserved because EAN/Greedy preserve
  // meaning under the client's declared law profile (the default profile is
  // universally safe); on any internal resource failure ean() falls back to the
  // original batch (root preservation, I3). The atoms are opaque to EAN and are
  // re-exported into the same factory (Graph.exprs()) that owns them, so the
  // evaluator consumes the optimized expressions unchanged.
  void applySummaryPostPass(
      PathSummaryEquationResult<ContextKey, atom_t> &Summary) {
    auto &Sums = Summary.summaries(); // non-const: rewrite values in place
    if (Sums.empty()) {
      return;
    }
    std::vector<ContextKey> Keys;
    std::vector<expr_ref_t> Roots;
    Keys.reserve(Sums.size());
    Roots.reserve(Sums.size());
    for (auto &KV : Sums) {
      Keys.push_back(KV.first);
      Roots.push_back(KV.second);
    }

    DiagnosticsValue.summary_before = ean::computeDagStats<atom_t>(Roots);
    DiagnosticsValue.summary_after = DiagnosticsValue.summary_before;

    const auto &EAN = Options.EAN;
    if (!EAN.EnableEAN && !EAN.EnableGreedy) {
      return; // baseline: no post-pass
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
    DiagnosticsValue.norm_time_us += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - NormStart)
            .count());

    if (Optimized.size() != Roots.size()) {
      return; // shape mismatch: keep the original summaries (I3)
    }
    for (std::size_t I = 0; I < Keys.size(); ++I) {
      Sums[Keys[I]] = Optimized[I];
    }
    DiagnosticsValue.summary_after = ean::computeDagStats<atom_t>(Optimized);
  }

  void evaluateSummaries(
      const PathSummaryEquationResult<ContextKey, atom_t> &Summary) {
    const auto Start = std::chrono::steady_clock::now();
    if (!HaveInitialFact) {
      InitialFact = Problem.bottom();
    }

    for (const auto &Entry : Summary.summaries()) {
      InterSummaryTransferEvaluator<AnalysisTypesT, K> Evaluator(
          Problem, *ICF, Result, Entry.first.Ctx);
      auto In = Evaluator.evaluateExpr(Entry.second, InitialFact);
      DiagnosticsValue.semantic_star_time_ns += Evaluator.semanticStarTimeNs();
      DiagnosticsValue.star_iterations_total += Evaluator.starIterations();
      Result.IN(Entry.first.Inst, Entry.first.Ctx) = In;

      auto Out = Problem.applyTransfer(
          Problem.edgeTransfer(Entry.first.Inst, n_t{}), In);
      Result.OUT(Entry.first.Inst, Entry.first.Ctx) = std::move(Out);
    }
    DiagnosticsValue.interp_time_us += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - Start)
            .count());
  }

  ProblemTy &Problem;
  PathSummaryEquationOptions Options;
  const i_t *ICF = nullptr;
  summary_graph_t Graph;
  std::unordered_set<ContextKey, ContextKeyHash> Discovered;
  std::unordered_map<n_t, fact_t> SeedFacts;
  fact_t InitialFact{};
  bool HaveInitialFact = false;
  result_t Result;
  bool HaveResult = false;
  SolveStatus LastStatus = SolveStatus::Ok;
  Diagnostics DiagnosticsValue;
};

} // namespace elimination

