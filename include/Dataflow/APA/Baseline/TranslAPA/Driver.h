#pragma once

// TranslAPA baseline — driver glue.
//
// The framework's elimination front-end already produces one path expression
// per node (Results.ExprTo(N), a hash-consed PathExprFactory DAG). This is the
// paper's path expression p. The baseline keeps that front-end verbatim and
// only swaps the interpreter: instead of SolverContext::eval (generic transfer
// re-application with Star iterated to a fixpoint), it folds p with the closed-
// form Gen/Kill semiring (paper §4) in a single memoized bottom-up pass.
//
// foldFillGenKill() is the reusable core: given a solved result and a
// translator that maps atoms to (Gen, Kill), it overwrites each node's IN fact
// with the semiring interpretation and returns the fold wall-clock time (µs).
// Client wrappers (e.g. reaching definitions) build the translator and call it.

#include "Dataflow/APA/Baseline/TranslAPA/FoldInterpreter.h"
#include "Dataflow/APA/Baseline/TranslAPA/GenKillSemiring.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Core/Result.h"

#include <chrono>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace elimination {
namespace translapa {

// Translator concept (duck-typed): must provide
//   unsigned      universeSize() const;
//   GenKillFact   translate(const transfer_t&) const;   // mechanical (Gen,Kill)
//   llvm::BitVector toBits(const fact_t&) const;         // fact -> universe bits
//   fact_t        fromBits(const llvm::BitVector&) const;// universe bits -> fact
//
// Overwrites Results.IN(N) for every node whose path expression exists; returns
// fold time in microseconds. Unreachable nodes (no ExprTo) are left untouched.
template <typename Domain, typename Translator>
std::size_t foldFillGenKill(
    const IntraEliminationProblem<Domain> &P,
    DataFlowResultT<typename Domain::n_t, typename Domain::fact_t,
                    typename Domain::transfer_t> &Results,
    const Translator &Tr) {
  using transfer_t = typename Domain::transfer_t;

  GenKillSemiring S(Tr.universeSize());
  auto Interp = makeFoldInterpreter<transfer_t>(
      S, [&Tr](const transfer_t &T) { return Tr.translate(T); });

  const llvm::BitVector Init = Tr.toBits(P.initialFact());
  const auto &ConstResults = Results; // use the non-inserting const ExprTo

  const auto Start = std::chrono::steady_clock::now();
  for (const auto &N : P.nodes()) {
    auto E = ConstResults.ExprTo(N);
    if (!E) {
      continue;
    }
    GenKillFact Summary = Interp.fold(E);
    Results.IN(N) = Tr.fromBits(S.apply(Summary, Init));
  }
  return static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - Start)
          .count());
}

// Split timing: mechanical extraction (one-time construction, paper's
// "construction") vs the per-query closed-form fold ("evaluation"). Warms the
// translator's atom cache in a first pass, then folds with cache hits so the
// two costs are measured apart. Returns {extract_us, fold_us}.
struct FoldTimings final {
  std::size_t extract_us = 0;
  std::size_t fold_us = 0;
};

template <typename Domain, typename Translator>
FoldTimings foldFillGenKillTimed(
    const IntraEliminationProblem<Domain> &P,
    DataFlowResultT<typename Domain::n_t, typename Domain::fact_t,
                    typename Domain::transfer_t> &Results,
    const Translator &Tr) {
  using transfer_t = typename Domain::transfer_t;
  using Factory = PathExprFactory<transfer_t>;
  using Expr = typename Factory::Expr;
  using Kind = typename Factory::Kind;

  const auto &ConstResults = Results;

  // Pass 1: warm the atom cache over the unique DAG nodes (extraction).
  FoldTimings Out;
  const auto ExtractStart = std::chrono::steady_clock::now();
  {
    std::unordered_map<const Expr *, bool> Seen;
    std::vector<const Expr *> Stack;
    for (const auto &N : P.nodes()) {
      auto E = ConstResults.ExprTo(N);
      if (E) {
        Stack.push_back(E.get());
      }
    }
    while (!Stack.empty()) {
      const Expr *E = Stack.back();
      Stack.pop_back();
      if (E == nullptr || !Seen.emplace(E, true).second) {
        continue;
      }
      switch (E->K) {
      case Kind::Atom:
        (void)Tr.translate(*E->Transfer);
        break;
      case Kind::Union:
      case Kind::Concat:
        Stack.push_back(E->L.get());
        Stack.push_back(E->R.get());
        break;
      case Kind::Star:
        Stack.push_back(E->L.get());
        break;
      default:
        break;
      }
    }
  }
  Out.extract_us = static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - ExtractStart)
          .count());

  // Pass 2: fold with a warm cache (evaluation).
  GenKillSemiring S(Tr.universeSize());
  auto Interp = makeFoldInterpreter<transfer_t>(
      S, [&Tr](const transfer_t &T) { return Tr.translate(T); });
  const llvm::BitVector Init = Tr.toBits(P.initialFact());
  const auto FoldStart = std::chrono::steady_clock::now();
  for (const auto &N : P.nodes()) {
    auto E = ConstResults.ExprTo(N);
    if (!E) {
      continue;
    }
    GenKillFact Summary = Interp.fold(E);
    Results.IN(N) = Tr.fromBits(S.apply(Summary, Init));
  }
  Out.fold_us = static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - FoldStart)
          .count());
  return Out;
}

} // namespace translapa
} // namespace elimination

