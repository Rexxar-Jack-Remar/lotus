#pragma once

#include "Dataflow/APA/Core/InterProblem.h"
#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Solver/Inter/Interpreter.h"
#include "Dataflow/APA/Solver/Intra/IntraSolver.h"
#include "Dataflow/Mono/Core/CallStringContext.h"

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace elimination {

template <typename AnalysisTypesT, unsigned K>
class InterEliminationSolver final {
public:
  using ProblemTy = InterEliminationProblem<AnalysisTypesT>;
  using fact_t = typename AnalysisTypesT::fact_t;
  using n_t = typename AnalysisTypesT::n_t;
  using f_t = typename AnalysisTypesT::f_t;
  using transfer_t = typename AnalysisTypesT::transfer_t;
  using i_t = typename AnalysisTypesT::i_t;
  using result_t = InterDataFlowResultT<K, fact_t, transfer_t, n_t>;
  using Context = mono::CallStringCTX<n_t, K>;

  struct ProcedureContextKey final {
    f_t Function{};
    Context Ctx;

    bool operator<(const ProcedureContextKey &Other) const {
      if (Function != Other.Function) {
        return std::less<f_t>{}(Function, Other.Function);
      }
      return Ctx < Other.Ctx;
    }
  };

  struct CallLink final {
    ProcedureContextKey Caller;
    n_t CallSite{};

    bool operator<(const CallLink &Other) const {
      if (Caller < Other.Caller)
        return true;
      if (Other.Caller < Caller)
        return false;
      return std::less<n_t>{}(CallSite, Other.CallSite);
    }
  };

  explicit InterEliminationSolver(
      ProblemTy &Problem,
      EliminationOptions ProcedureOptions = defaultProcedureOptions())
      : Problem(Problem), ProcedureOptions(ProcedureOptions) {}

  SolveStatus solve() {
    Diagnostics = {};
    LastStatus = SolveStatus::Ok;
    loadSeedFacts();
    const auto *ICF = Problem.getICFG();
    if (ICF == nullptr) {
      HaveResult = false;
      return LastStatus = SolveStatus::InvalidProblem;
    }

    Result = result_t{};
    Result.setMissingFactFallback(Problem.bottom());
    HaveResult = true;

    IncomingCalls.clear();
    LastBoundaries.clear();
    ActiveCalls.clear();
    SolvedContexts.clear();
    SummaryCache.clear();
    std::deque<ProcedureContextKey> Worklist;
    std::set<ProcedureContextKey> InQueue;

    auto Enqueue = [&](ProcedureContextKey Key) {
      if (InQueue.insert(Key).second) {
        Worklist.push_back(std::move(Key));
      }
    };

    Context EmptyCtx;
    for (const auto &Seed : SeedFacts) {
      Result.IN(Seed.first, EmptyCtx) = Seed.second;
      auto Function = ICF->getFunctionOf(Seed.first);
      if (Function != f_t{})
        Enqueue({Function, EmptyCtx});
    }

    if (SeedFacts.empty()) {
      for (auto Entry : Problem.getEntryPoints()) {
        if (Entry == f_t{}) {
          continue;
        }
        if (Problem.direction() ==
            ::dataflow::controlflow::FlowDirection::Backward) {
          if (!ICF->getExitPointsOf(Entry).empty())
            Enqueue({Entry, EmptyCtx});
        } else {
          auto Starts = ICF->getStartPointsOf(Entry);
          if (!Starts.empty() && Starts.front() != n_t{})
            Enqueue({Entry, EmptyCtx});
        }
      }
    }

    while (!Worklist.empty()) {
      auto Key = Worklist.front();
      Worklist.pop_front();
      InQueue.erase(Key);

      const bool FirstSolve = SolvedContexts.insert(Key).second;
      const bool Changed = solveProcedureForContext(Key, *ICF);
      if (LastStatus == SolveStatus::InvalidProblem ||
          LastStatus == SolveStatus::NonConvergentStar) {
        HaveResult = LastStatus != SolveStatus::InvalidProblem;
        Result.setSolveStatus(LastStatus);
        Result.setContextSolveDiagnostics(Diagnostics);
        publishProcedureDiagnostics();
        return LastStatus;
      }
      if (Changed || FirstSolve)
        scheduleAdjacentProcedures(Key, *ICF, Enqueue);
    }

    Result.setSolveStatus(SolveStatus::Ok);
    Diagnostics.procedure_context_count = SummaryCache.size();
    Result.setContextSolveDiagnostics(Diagnostics);
    publishProcedureDiagnostics();
    return LastStatus = SolveStatus::Ok;
  }

  const result_t *getResults() const { return HaveResult ? &Result : nullptr; }
  SolveStatus getLastStatus() const { return LastStatus; }
  // Construction is counted once per procedure/context; interpretation and
  // semantic iteration are accumulated over all fact-propagation epochs.
  const SolveDiagnostics &getDiagnostics() const { return Diagnostics; }

private:
  class ProcedureProblemAdapter final
      : public IntraEliminationProblem<AnalysisTypesT> {
  public:
    ProcedureProblemAdapter(ProblemTy &Problem, f_t Function,
                            const Context &Ctx, const result_t &Result,
                            const i_t &ICF, const fact_t &EntryFact)
        : Problem(Problem), Function(Function), Ctx(Ctx), Result(Result),
          ICF(ICF), EntryFact(EntryFact) {}

    std::vector<n_t> nodes() const override {
      if (Function == f_t{}) {
        return {};
      }
      return ICF.getAllInstructionsOf(Function);
    }

    n_t entry() const override {
      if (Function == f_t{}) {
        return n_t{};
      }
      if (Problem.direction() ==
          ::dataflow::controlflow::FlowDirection::Backward) {
        auto Exits = ICF.getExitPointsOf(Function);
        return Exits.empty() ? n_t{} : Exits.front();
      }
      auto Starts = ICF.getStartPointsOf(Function);
      return Starts.empty() ? n_t{} : Starts.front();
    }

    std::vector<n_t> succs(n_t Node) const override {
      return ICF.getSuccsOf(Node, Problem.direction());
    }

    transfer_t edgeTransfer(n_t Src, n_t Dst) const override {
      return Problem.edgeTransfer(Src, Dst);
    }

    fact_t applyTransfer(const transfer_t &T, const fact_t &In) const override {
      InterSummaryTransferEvaluator<AnalysisTypesT, K> Evaluator(Problem, ICF,
                                                                 Result, Ctx);
      return Evaluator.applyNormalEdge(T, In);
    }

    fact_t join(const fact_t &Lhs, const fact_t &Rhs) const override {
      return Problem.join(Lhs, Rhs);
    }

    bool equal(const fact_t &Lhs, const fact_t &Rhs) const override {
      return Problem.equal(Lhs, Rhs);
    }

    fact_t bottom() const override { return Problem.bottom(); }
    fact_t initialFact() const override { return EntryFact; }

  private:
    ProblemTy &Problem;
    f_t Function{};
    Context Ctx;
    const result_t &Result;
    const i_t &ICF;
    fact_t EntryFact;
  };

  void loadSeedFacts() { SeedFacts = Problem.initialSeeds(); }

  bool solveProcedureForContext(const ProcedureContextKey &Key,
                                const i_t &ICF) {
    auto Function = Key.Function;
    if (Function == f_t{} || ICF.getStartPointsOf(Function).empty()) {
      return false;
    }

    auto EntryFact = boundaryFactForContext(Function, Key.Ctx, ICF);
    LastBoundaries[Key] = EntryFact;
    ProcedureProblemAdapter Adapter(Problem, Function, Key.Ctx, Result, ICF,
                                    EntryFact);
    auto Cached = SummaryCache.find(Key);
    if (Cached == SummaryCache.end()) {
      auto BuildOptions = ProcedureOptions;
      BuildOptions.InterpMemo =
          true; // Build/normalize expressions, not boundary-specific facts.
      IntraEliminationSolver<AnalysisTypesT> Builder(Adapter, BuildOptions);
      const auto Status = Builder.solve();
      const auto &D = Builder.getDiagnostics();
      Diagnostics.ordering.append(D.ordering);
      Diagnostics.gen_time_us += D.gen_time_us;
      Diagnostics.norm_time_us += D.norm_time_us;
      Diagnostics.peak_matrix_nodes =
          std::max(Diagnostics.peak_matrix_nodes, D.peak_matrix_nodes);
      if (Status == SolveStatus::InvalidProblem) {
        LastStatus = Status;
        return false;
      }
      Cached = SummaryCache
                   .emplace(Key, CachedProcedure{Builder.getResults(),
                                                 Adapter.nodes(),
                                                 Adapter.entry(), D})
                   .first;
      ++Diagnostics.procedure_summary_builds;
    } else {
      ++Diagnostics.procedure_summary_reuses;
      ++Cached->second.Diagnostics.procedure_summary_reuses;
    }
    Diagnostics.procedure_context_count = SummaryCache.size();

    // External call/return facts are stable until all query facts have been
    // evaluated. A fresh input-sensitive memo is used in each epoch so
    // recursive callee updates cannot reuse stale facts from an earlier
    // propagation pass.
    auto InterpretOptions = ProcedureOptions;
    InterpretOptions.MemoizeInterpretation = true;
    bool Failed = false;
    SolveDiagnostics Epoch;
    detail::FactInterpreter<ProcedureProblemAdapter> Interpreter(
        Adapter, InterpretOptions, Epoch, Failed);
    auto ProcRes = Cached->second.Expressions;
    const auto InterpStart = std::chrono::steady_clock::now();
    for (auto Node : Cached->second.Nodes) {
      ProcRes.IN(Node) = Interpreter.eval(ProcRes.ExprTo(Node), EntryFact);
    }
    Epoch.interp_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - InterpStart)
            .count();
    Diagnostics.interp_time_us += Epoch.interp_time_us;
    Diagnostics.star_iterations_total += Epoch.star_iterations_total;
    Diagnostics.semantic_star_time_ns += Epoch.semantic_star_time_ns;
    Diagnostics.max_star_hit |= Epoch.max_star_hit;
    auto &PerPair = Cached->second.Diagnostics;
    PerPair.interp_time_us += Epoch.interp_time_us;
    PerPair.star_iterations_total += Epoch.star_iterations_total;
    PerPair.semantic_star_time_ns += Epoch.semantic_star_time_ns;
    PerPair.max_star_hit |= Epoch.max_star_hit;
    PerPair.procedure_summary_builds = 1;
    if (Failed) {
      LastStatus = SolveStatus::NonConvergentStar;
      return false;
    }

    bool Changed = false;
    const auto &Nodes = Cached->second.Nodes;
    auto &Calls = ActiveCalls[Key];
    Calls.clear();
    for (auto Inst : Nodes) {
      auto Expr = ProcRes.ExprTo(Inst);
      if (ICF.isCallSite(Inst) && Expr &&
          !PathExprFactory<transfer_t>::isZero(Expr))
        Calls.insert(Inst);
      const auto *In = ProcRes.tryIN(Inst);
      if (In == nullptr) {
        continue;
      }
      auto &InSlot = Result.IN(Inst, Key.Ctx);
      if (!Problem.equal(InSlot, *In)) {
        InSlot = *In;
        Changed = true;
      }

      auto OutTransfer =
          Problem.direction() == dataflow::controlflow::FlowDirection::Backward
              ? Problem.edgeTransfer(n_t{}, Inst)
              : Problem.edgeTransfer(Inst, n_t{});
      auto Out = Problem.applyTransfer(OutTransfer, *In);
      auto &OutSlot = Result.OUT(Inst, Key.Ctx);
      if (!Problem.equal(OutSlot, Out)) {
        OutSlot = std::move(Out);
        Changed = true;
      }
    }

    return Changed;
  }

  fact_t boundaryFactForContext(f_t Function, const Context &Ctx,
                                const i_t &ICF) {
    if (Function == f_t{})
      return Problem.bottom();

    auto Starts = ICF.getStartPointsOf(Function);
    auto Exits = ICF.getExitPointsOf(Function);
    if (Starts.empty() && Exits.empty())
      return Problem.bottom();

    fact_t Boundary = Problem.bottom();
    bool HaveBoundary = false;
    auto MergeBoundary = [&](fact_t Fact) {
      if (!HaveBoundary) {
        Boundary = std::move(Fact);
        HaveBoundary = true;
      } else {
        Boundary = Problem.join(Boundary, Fact);
      }
    };

    const ProcedureContextKey CalleeKey{Function, Ctx};
    auto LinksIt = IncomingCalls.find(CalleeKey);
    auto EntryInst = Starts.empty() ? n_t{} : Starts.front();
    if (Problem.direction() ==
        ::dataflow::controlflow::FlowDirection::Backward) {
      if (Ctx.empty()) {
        for (auto Exit : Exits) {
          auto It = SeedFacts.find(Exit);
          if (Exit != n_t{} && It != SeedFacts.end())
            MergeBoundary(It->second);
        }
      }

      if (LinksIt != IncomingCalls.end()) {
        for (const auto &Link : LinksIt->second) {
          for (auto RetSite : ICF.getReturnSitesOfCallAt(Link.CallSite)) {
            if (RetSite == n_t{})
              continue;
            auto *RetFacts = Result.tryOUT(RetSite, Link.Caller.Ctx);
            if (RetFacts == nullptr)
              continue;
            for (auto Exit : Exits) {
              if (Exit == n_t{})
                continue;
              MergeBoundary(Problem.returnFlow(Link.CallSite, Function, Exit,
                                               RetSite, *RetFacts));
            }
          }
        }
      }
      return HaveBoundary ? Boundary : Problem.bottom();
    }

    if (Ctx.empty()) {
      auto It = SeedFacts.find(EntryInst);
      if (It != SeedFacts.end())
        MergeBoundary(It->second);
    }

    if (LinksIt != IncomingCalls.end()) {
      for (const auto &Link : LinksIt->second) {
        auto *CallerFacts = Result.tryOUT(Link.CallSite, Link.Caller.Ctx);
        if (CallerFacts == nullptr)
          continue;
        InterSummaryTransferEvaluator<AnalysisTypesT, K> Evaluator(
            Problem, ICF, Result, Link.Caller.Ctx);
        MergeBoundary(
            Evaluator.applyCallEntry(Link.CallSite, Function, *CallerFacts));
      }
    }
    return HaveBoundary ? Boundary : Problem.bottom();
  }

  template <typename EnqueueT>
  void scheduleAdjacentProcedures(const ProcedureContextKey &Key,
                                  const i_t &ICF, EnqueueT &&Enqueue) {
    auto CallsIt = ActiveCalls.find(Key);
    if (CallsIt != ActiveCalls.end()) {
      for (auto Inst : CallsIt->second) {
        Context CalleeCtx = Key.Ctx;
        CalleeCtx.push_back(Inst);
        for (auto Callee : ICF.getCalleesOfCallAt(Inst)) {
          if (Callee == f_t{} || ICF.getStartPointsOf(Callee).empty() ||
              ICF.getExitPointsOf(Callee).empty())
            continue;
          ProcedureContextKey CalleeKey{Callee, CalleeCtx};
          IncomingCalls[CalleeKey].insert(CallLink{Key, Inst});
          auto Boundary = boundaryFactForContext(Callee, CalleeCtx, ICF);
          auto LastIt = LastBoundaries.find(CalleeKey);
          if (LastIt == LastBoundaries.end() ||
              !Problem.equal(LastIt->second, Boundary))
            Enqueue(CalleeKey);
        }
      }
    }

    auto LinksIt = IncomingCalls.find(Key);
    if (LinksIt != IncomingCalls.end()) {
      for (const auto &Link : LinksIt->second)
        Enqueue(Link.Caller);
    }
  }

  ProblemTy &Problem;
  struct CachedProcedure {
    DataFlowResultT<n_t, fact_t, transfer_t> Expressions;
    std::vector<n_t> Nodes;
    n_t Boundary{};
    SolveDiagnostics Diagnostics;
  };
  void publishProcedureDiagnostics() {
    std::vector<ProcedureContextDiagnostics<K, n_t>> Records;
    for (const auto &Pair : SummaryCache) {
      Records.push_back({Pair.second.Boundary, Pair.first.Ctx,
                         Pair.second.Nodes, Pair.second.Diagnostics});
    }
    Result.setProcedureContextDiagnostics(std::move(Records));
  }
  std::map<ProcedureContextKey, CachedProcedure> SummaryCache;
  EliminationOptions ProcedureOptions;
  result_t Result;
  bool HaveResult = false;
  SolveStatus LastStatus = SolveStatus::Ok;
  SolveDiagnostics Diagnostics;
  std::unordered_map<n_t, fact_t> SeedFacts;
  std::map<ProcedureContextKey, std::set<CallLink>> IncomingCalls;
  std::map<ProcedureContextKey, fact_t> LastBoundaries;
  std::map<ProcedureContextKey, std::set<n_t>> ActiveCalls;
  std::set<ProcedureContextKey> SolvedContexts;

  static EliminationOptions defaultProcedureOptions() {
    EliminationOptions Options;
    Options.Method = EliminationMethod::ADTSimple;
    return Options;
  }
};

} // namespace elimination

