#pragma once

#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Solver/Inter/Modular/SummaryBuilder.h"

#include <cstddef>
#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace elimination {

// Interpretation for the modular interprocedural summaries (E6, milestone 4).
//
// Evaluates the per-procedure entry->node path expressions into client facts.
// The only non-local case is the SummaryCall atom: In -> callFlow -> evaluate
// the callee's entry->exit summary -> returnFlow. Recursion (a SummaryCall to a
// procedure whose evaluation is already on the stack) is closed by a memoized
// fixpoint (E1): each (callee, exit, input-fact) is evaluated at most once per
// outer pass; a re-entrant demand returns the current approximation; outer
// passes repeat until no tabulated value changes (bounded by a cap). This is
// context-insensitive / functional (D1=A): one merged entry fact per procedure,
// hence one IN per instruction (empty context key, F1).
template <typename AnalysisDomainTy, unsigned K>
class ModularInterSummaryInterpreter final {
public:
  using ProblemTy = InterEliminationProblem<AnalysisDomainTy>;
  using fact_t = typename AnalysisDomainTy::fact_t;
  using n_t = typename AnalysisDomainTy::n_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using transfer_t = typename AnalysisDomainTy::transfer_t;
  using i_t = typename AnalysisDomainTy::i_t;
  using atom_t = InterSummaryTransferAtom<AnalysisDomainTy>;
  using expr_factory_t = PathExprFactory<atom_t>;
  using expr_ref_t = typename expr_factory_t::Ref;
  using builder_t = ModularInterSummaryBuilder<AnalysisDomainTy>;
  using SummaryTable = typename builder_t::SummaryTable;
  using result_t = InterDataFlowResultT<K, fact_t, transfer_t, n_t>;
  using Context = typename result_t::Context;

  ModularInterSummaryInterpreter(ProblemTy &Problem, const i_t &ICF,
                                 const SummaryTable &Summaries)
      : Problem(Problem), ICF(ICF), Summaries(Summaries) {}

  // Evaluate one procedure's summary at input fact In, evaluating SummaryCall
  // atoms through the tabulated callee summaries.
  fact_t evalExpr(const expr_ref_t &Expr, const fact_t &In) {
    if (!Expr) {
      return Problem.bottom();
    }
    switch (Expr->K) {
    case expr_factory_t::Kind::Zero:
      return Problem.bottom();
    case expr_factory_t::Kind::One:
      return In;
    case expr_factory_t::Kind::Atom:
      return applyAtom(*Expr->Transfer, In);
    case expr_factory_t::Kind::Union: {
      auto L = evalExpr(Expr->L, In);
      auto R = evalExpr(Expr->R, In);
      return Problem.join(L, R);
    }
    case expr_factory_t::Kind::Concat: {
      auto Mid = evalExpr(Expr->L, In);
      return evalExpr(Expr->R, Mid);
    }
    case expr_factory_t::Kind::Star: {
      detail::ScopedNestedNanoseconds Timer(SemanticStarTimeNs, StarDepth);
      auto Cur = In;
      for (std::size_t I = 0; I < kMaxStarIterations; ++I) {
        ++StarIterations;
        auto Next = Problem.join(In, evalExpr(Expr->L, Cur));
        if (Problem.equal(Next, Cur)) {
          return Cur;
        }
        Cur = std::move(Next);
      }
      return Cur;
    }
    }
    return Problem.bottom();
  }

  bool changedThisPass() const { return Changed; }
  std::uint64_t semanticStarTimeNs() const { return SemanticStarTimeNs; }
  std::size_t starIterations() const { return StarIterations; }
  void beginPass() {
    Changed = false;
    ++CurPass;
  }

private:
  static constexpr std::size_t kMaxStarIterations = 100000;

  fact_t applyAtom(const atom_t &A, const fact_t &In) {
    switch (A.K) {
    case atom_t::Kind::RawNormal:
      return Problem.applyTransfer(A.NormalTransfer, In);
    case atom_t::Kind::CallToRet:
      return Problem.callToRetFlow(A.CallSite, A.RetSite, A.Callees, In);
    case atom_t::Kind::SummaryCall:
      return applySummaryCall(A.CallSite, A.Callee, A.RetSite, In);
    case atom_t::Kind::Normal:
    case atom_t::Kind::CallEntry:
    case atom_t::Kind::ReturnExit:
      // These are not produced by the modular per-procedure graph.
      return Problem.bottom();
    }
    return Problem.bottom();
  }

  // In -> callFlow -> callee entry->exit_i -> returnFlow, unioned over exits.
  fact_t applySummaryCall(n_t CallSite, f_t Callee, n_t RetSite,
                          const fact_t &In) {
    auto SumIt = Summaries.find(Callee);
    if (SumIt == Summaries.end()) {
      return Problem.bottom(); // opaque/unknown callee: no summary
    }
    const fact_t CallIn = Problem.callFlow(CallSite, Callee, In);
    fact_t Out = Problem.bottom();
    for (const auto &Exit : ICF.getExitPointsOf(Callee)) {
      if (Exit == n_t{}) {
        continue;
      }
      auto NodeIt = SumIt->second.perNode.find(Exit);
      if (NodeIt == SumIt->second.perNode.end()) {
        continue;
      }
      const fact_t Ei = procApply(Callee, Exit, NodeIt->second, CallIn);
      Out = Problem.join(
          Out, Problem.returnFlowWithCallerFact(CallSite, Callee, Exit, RetSite,
                                                Ei, In));
    }
    return Out;
  }

  struct Entry final {
    fact_t In;
    fact_t Out;
    bool computing = false;
    std::size_t visitedPass = 0;
  };

  // Memoized fixpoint (E1). Recompute at most once per outer pass; a re-entrant
  // demand (computing) returns the current approximation to break recursion.
  fact_t procApply(f_t Callee, n_t Exit, const expr_ref_t &ExitExpr,
                   const fact_t &In) {
    auto &Bucket = Tab[std::make_pair(Callee, Exit)];
    for (auto &E : Bucket) {
      if (Problem.equal(E.In, In)) {
        if (E.computing || E.visitedPass == CurPass) {
          return E.Out; // recursion cut, or already refreshed this pass
        }
        E.computing = true;
        fact_t Val = evalExpr(ExitExpr, In);
        E.computing = false;
        E.visitedPass = CurPass;
        if (!Problem.equal(E.Out, Val)) {
          E.Out = std::move(Val);
          Changed = true;
        }
        return E.Out;
      }
    }
    Bucket.push_back(Entry{In, Problem.bottom(), true, CurPass});
    Entry &Cur = Bucket.back(); // std::deque: stable across later push_back
    fact_t Val = evalExpr(ExitExpr, In);
    Cur.computing = false;
    if (!Problem.equal(Cur.Out, Val)) {
      Cur.Out = std::move(Val);
      Changed = true;
    }
    return Cur.Out;
  }

  ProblemTy &Problem;
  const i_t &ICF;
  const SummaryTable &Summaries;
  std::map<std::pair<f_t, n_t>, std::deque<Entry>> Tab;
  std::size_t CurPass = 0;
  bool Changed = false;
  std::uint64_t SemanticStarTimeNs = 0;
  std::size_t StarIterations = 0;
  std::size_t StarDepth = 0;
};

} // namespace elimination

