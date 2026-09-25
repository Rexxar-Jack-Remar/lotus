#pragma once

#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/Solver/Ordering/StructuralModel.h"
#include "Dataflow/APA/Solver/Intra/Context.h"
#include "Dataflow/APA/Solver/Elimination/SparseSolver.h"
#include "Dataflow/APA/Solver/Elimination/DenseClosure.h"

#include <chrono>

namespace elimination {
namespace detail {

// Build the boolean elimination graph from the current matrix's off-diagonal
// nonzeros (direct CFG edges at this point). Self-loops are the diagonal
// (always one()) and are intentionally excluded — they do not affect the
// predecessor–successor product.
template <typename AnalysisDomainTy>
order::EliminationGraph buildEliminationGraph(
    const IntraEliminationSolverContext<AnalysisDomainTy> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisDomainTy>;
  const auto N = Ctx.Nodes.size();
  order::EliminationGraph G(N);
  for (std::size_t i = 0; i < N; ++i) {
    for (std::size_t j = 0; j < N; ++j) {
      if (i == j) {
        continue;
      }
      if (!Context::expr_factory_t::isZero(Ctx.Matrix[i][j])) {
        G.addEdge(i, j);
      }
    }
  }
  return G;
}

// Generic Floyd-Warshall-style elimination over the full CFG. This engine
// makes no reducibility assumptions and therefore serves as the baseline
// implementation as well as the fallback when ADT-specific preconditions fail.
template <typename AnalysisTypesT>
std::vector<std::size_t> getStateEliminationOrder(
    const IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisTypesT>;
  const auto N = Ctx.Nodes.size();

  // Cost-aware policy: greedy minimum-product order over the elimination graph.
  // Fully replaces the baseline order (including the reducible reverse-topo
  // path). The final all-pairs result is invariant to pivot order.
  if (Ctx.Opts.Ordering == OrderingPolicy::CostAware) {
    return order::computeCostAwareOrder(buildEliminationGraph(Ctx));
  }

  std::vector<std::size_t> Order(N);
  const auto *R =
      dynamic_cast<const typename Context::ReducibleProblemTy *>(&Ctx.Problem);
  if (R != nullptr) {
    const auto Topo = R->topologicalOrder();
    if (Topo.size() == N) {
      for (std::size_t i = 0; i < N; ++i) {
        const auto It = Ctx.Index.find(Topo[N - 1 - i]);
        if (It == Ctx.Index.end()) {
          goto DefaultOrder;
        }
        Order[i] = It->second;
      }
      return Order;
    }
  }
DefaultOrder:
  for (std::size_t i = 0; i < N; ++i) {
    Order[i] = i;
  }
  return Order;
}

template <typename AnalysisTypesT>
void buildStateEliminationMatrix(
    IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  // Build the usual elimination matrix where M[i][j] summarizes all direct
  // edges from node i to node j. Diagonals start at one() so paths are allowed
  // to stay at a node before additional eliminations introduce loops.
  Ctx.Nodes = Ctx.Problem.nodes();
  Ctx.Index.clear();
  Ctx.Index.reserve(Ctx.Nodes.size());
  for (std::size_t i = 0; i < Ctx.Nodes.size(); ++i) {
    Ctx.Index.emplace(Ctx.Nodes[i], i);
  }

  const auto N = Ctx.Nodes.size();
  Ctx.Matrix.assign(
      N,
      std::vector<
          typename IntraEliminationSolverContext<AnalysisTypesT>::expr_ref_t>(
          N, Ctx.Exprs.zero()));
  for (std::size_t i = 0; i < N; ++i) {
    Ctx.Matrix[i][i] = Ctx.Exprs.one();
  }

  for (const auto &Src : Ctx.Nodes) {
    const auto SrcIdx = Ctx.idx(Src);
    for (const auto &Dst : Ctx.Problem.succs(Src)) {
      const auto It = Ctx.Index.find(Dst);
      if (It == Ctx.Index.end()) {
        continue;
      }
      const auto DstIdx = It->second;
      Ctx.Matrix[SrcIdx][DstIdx] =
          Ctx.Exprs.unite(Ctx.Matrix[SrcIdx][DstIdx],
                          Ctx.Exprs.atom(Ctx.Problem.edgeTransfer(Src, Dst)));
    }
  }
}

template <typename AnalysisTypesT>
void eliminateStateIntermediates(
    IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  using Context = IntraEliminationSolverContext<AnalysisTypesT>;
  using transfer_t = typename Context::transfer_t;
  const auto N = Ctx.Nodes.size();

  // Opt-in RQ3 instrumentation: peak unique DAG nodes across the whole matrix.
  const bool Measure = Ctx.Opts.MeasurePeakNodes;
  auto measurePeak = [&]() {
    if (!Measure) {
      return;
    }
    std::vector<typename Context::expr_ref_t> Live;
    Live.reserve(N * N);
    for (std::size_t i = 0; i < N; ++i) {
      for (std::size_t j = 0; j < N; ++j) {
        if (!Context::expr_factory_t::isZero(Ctx.Matrix[i][j])) {
          Live.push_back(Ctx.Matrix[i][j]);
        }
      }
    }
    const std::size_t nodes = ean::computeDagStats<transfer_t>(Live).uniqueNodes;
    if (nodes > Ctx.Diagnostics.peak_matrix_nodes) {
      Ctx.Diagnostics.peak_matrix_nodes = nodes;
    }
  };

  const auto Order = getStateEliminationOrder(Ctx);
  closeDenseMatrix(Ctx.Matrix, Ctx.Exprs, Order, [&](std::size_t) { measurePeak(); });
}

template <typename AnalysisTypesT>
bool materializeStateResults(
    IntraEliminationSolverContext<AnalysisTypesT> &Ctx,
    const std::vector<typename IntraEliminationSolverContext<
        AnalysisTypesT>::expr_ref_t> *Summaries = nullptr) {
  using Context = IntraEliminationSolverContext<AnalysisTypesT>;
  Ctx.Results = typename Context::result_t{};
  if (Ctx.Nodes.empty()) {
    return true;
  }

  const auto EntryIt = Ctx.Index.find(Ctx.Problem.entry());
  if (EntryIt == Ctx.Index.end()) {
    return false;
  }
  const auto EntryIdx = EntryIt->second;

  const auto Init = Ctx.Problem.initialFact();
  const std::size_t Reps = Ctx.Opts.InterpRepeat ? Ctx.Opts.InterpRepeat : 1;
  const auto InterpStart = std::chrono::steady_clock::now();
  for (std::size_t j = 0; j < Ctx.Nodes.size(); ++j) {
    const auto &N = Ctx.Nodes[j];
    // Each remaining matrix entry summarizes all paths from entry to N.
    auto E = Summaries ? (*Summaries)[j] : Ctx.Matrix[EntryIdx][j];
    Ctx.Results.ExprTo(N) = E;
    // Skip the interpretation when EAN or Greedy will re-optimize and
    // re-evaluate the whole batch afterwards (avoids a wasted eval), or when a
    // memoizing client interpreter (InterpMemo) will fill IN facts itself.
    if (!Ctx.Opts.EnableEAN && !Ctx.Opts.EnableGreedy && !Ctx.Opts.InterpMemo) {
      typename Context::fact_t V = Ctx.Interpreter.eval(E, Init);
      for (std::size_t r = 1; r < Reps; ++r) {
        V = Ctx.Interpreter.eval(E, Init); // amortization measurement (RQ2)
      }
      Ctx.Results.IN(N) = std::move(V);
    }
  }
  Ctx.Diagnostics.interp_time_us += static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - InterpStart)
          .count());
  return true;
}

template <typename AnalysisTypesT>
bool solveStateElimination(
    IntraEliminationSolverContext<AnalysisTypesT> &Ctx) {
  const auto GenStart = std::chrono::steady_clock::now();
  if (usesOnlineElimination(Ctx.Opts.Ordering) ||
      Ctx.Opts.Order.UseSparseElimination) {
    using Context = IntraEliminationSolverContext<AnalysisTypesT>;
    Ctx.Nodes = Ctx.Problem.nodes();
    Ctx.Index.clear();
    Ctx.Matrix.clear();
    for (std::size_t I = 0; I < Ctx.Nodes.size(); ++I) {
      if (!Ctx.Index.emplace(Ctx.Nodes[I], I).second) {
        return false;
      }
    }
    std::vector<typename Context::expr_ref_t> Base(Ctx.Nodes.size(),
                                                   Ctx.Exprs.zero());
    const auto Entry = Ctx.Index.find(Ctx.Problem.entry());
    if (!Ctx.Nodes.empty() && Entry == Ctx.Index.end()) {
      return false;
    }
    if (Entry != Ctx.Index.end()) {
      Base[Entry->second] = Ctx.Exprs.one();
    }
    auto Options = Ctx.Opts.Order;
    Options.MeasureLiveNodes |= Ctx.Opts.MeasurePeakNodes;
    std::vector<typename Context::expr_ref_t> Summaries;
    try {
      auto Policy = Ctx.Opts.Ordering;
      if (Policy == OrderingPolicy::Default) {
        Options.ExplicitOrder = getStateEliminationOrder(Ctx);
        Policy = OrderingPolicy::Explicit;
      } else if (Policy == OrderingPolicy::CostAware) {
        order::EliminationGraph Graph(Ctx.Nodes.size());
        for (std::size_t I = 0; I < Ctx.Nodes.size(); ++I) {
          for (const auto &Dst : Ctx.Problem.succs(Ctx.Nodes[I])) {
            auto Target = Ctx.Index.find(Dst);
            if (Target != Ctx.Index.end()) {
              Graph.addEdge(I, Target->second);
            }
          }
        }
        Options.ExplicitOrder = order::computeCostAwareOrder(Graph);
        Policy = OrderingPolicy::Explicit;
      }
      SparseElimination<typename Context::transfer_t> Solver(
          Ctx.Exprs, std::move(Base), Policy, Options);
      for (std::size_t I = 0; I < Ctx.Nodes.size(); ++I) {
        for (const auto &Dst : Ctx.Problem.succs(Ctx.Nodes[I])) {
          const auto Target = Ctx.Index.find(Dst);
          if (Target != Ctx.Index.end()) {
            Solver.addEdge(
                I, Target->second,
                Ctx.Exprs.atom(Ctx.Problem.edgeTransfer(Ctx.Nodes[I], Dst)));
          }
        }
      }
      Summaries = Solver.solve();
      Ctx.Diagnostics.ordering = Solver.diagnostics();
      Ctx.Diagnostics.peak_matrix_nodes = Solver.diagnostics().peak_live_nodes;
    } catch (const std::invalid_argument &) {
      return false;
    }
    Ctx.Diagnostics.gen_time_us += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - GenStart)
            .count());
    return materializeStateResults(Ctx, &Summaries);
  }
  buildStateEliminationMatrix(Ctx);
  eliminateStateIntermediates(Ctx);
  Ctx.Diagnostics.gen_time_us += static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - GenStart)
          .count());
  return materializeStateResults(Ctx);
}

} // namespace detail
} // namespace elimination

