#ifndef DATAFLOW_APA_TOOLING_APADIAGNOSTICS_H_
#define DATAFLOW_APA_TOOLING_APADIAGNOSTICS_H_

#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/Tooling/APAFormatting.h"
#include "Dataflow/Tooling/ToolSupport.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace elimination::tooling {

struct CFGStats final {
  size_t Arguments = 0;
  size_t Blocks = 0;
  size_t Instructions = 0;
  size_t Edges = 0;
  size_t BranchingBlocks = 0;
  size_t MaxSuccessors = 0;
  size_t PhiNodes = 0;
  size_t Calls = 0;
  size_t Returns = 0;
  size_t Unreachable = 0;
};

CFGStats collectCFGStats(const llvm::Function &F);

struct ExprProfile final {
  size_t UniqueNodes = 0;
  size_t SharedRefs = 0;
  size_t MaxDepth = 0;
  size_t ZeroNodes = 0;
  size_t OneNodes = 0;
  size_t AtomNodes = 0;
  size_t UnionNodes = 0;
  size_t ConcatNodes = 0;
  size_t StarNodes = 0;
};

template <typename ExprRefT>
void collectExprProfileImpl(const ExprRefT &Expr, size_t Depth,
                            std::unordered_set<const void *> &Visited,
                            ExprProfile &Profile) {
  if (!Expr)
    return;

  Profile.MaxDepth = std::max(Profile.MaxDepth, Depth);
  if (!Visited.insert(Expr.get()).second) {
    ++Profile.SharedRefs;
    return;
  }

  ++Profile.UniqueNodes;
  using Kind = decltype(Expr->K);
  switch (Expr->K) {
  case Kind::Zero:
    ++Profile.ZeroNodes;
    return;
  case Kind::One:
    ++Profile.OneNodes;
    return;
  case Kind::Atom:
    ++Profile.AtomNodes;
    return;
  case Kind::Union:
    ++Profile.UnionNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case Kind::Concat:
    ++Profile.ConcatNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case Kind::Star:
    ++Profile.StarNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    return;
  }
}

template <typename ExprRefT>
ExprProfile collectExprProfile(const ExprRefT &Expr) {
  ExprProfile Profile;
  std::unordered_set<const void *> Visited;
  collectExprProfileImpl(Expr, 1, Visited, Profile);
  return Profile;
}

void emitOrderingDiagnostics(llvm::raw_ostream &OS,
                             const elimination::OrderingDiagnostics &D);

template <typename ResultT>
void recordSolveStatus(const ResultT &Result, bool &HadSolveError) {
  if (Result.hasSolveMetadata()) {
    HadSolveError |=
        Result.solveStatus() == elimination::SolveStatus::InvalidProblem ||
        Result.solveStatus() == elimination::SolveStatus::NonConvergentStar;
  }
}

template <typename ResultT>
void printSolveMetadata(llvm::raw_ostream &OS, const ResultT &Result,
                        bool &HadSolveError) {
  if (!Result.hasSolveMetadata())
    return;
  const auto &Diag = Result.solveDiagnostics();
  recordSolveStatus(Result, HadSolveError);
  OS << "  [solver] status=" << toString(Result.solveStatus())
     << ", requested=" << toString(Diag.requested_method)
     << ", executed=" << toString(Diag.executed_method)
     << ", used_adt=" << (Diag.used_adt ? "true" : "false")
     << ", fallback=" << toString(Diag.fallback_reason)
     << ", adt_reason=" << toString(Diag.adt_rejection_reason)
     << ", star_iters=" << Diag.star_iterations_total
     << ", max_star_hit=" << (Diag.max_star_hit ? "true" : "false")
     << ", ean_laws_restricted="
     << (Diag.ean_laws_restricted ? "true" : "false")
     << ", peak_nodes=" << Diag.peak_matrix_nodes
     << ", semantic_star_ns=" << Diag.semantic_star_time_ns << "\n";
  emitOrderingDiagnostics(OS, Diag.ordering);
}

template <unsigned K, typename FactT, typename TransferT, typename NodeT>
void printSolveMetadata(
    llvm::raw_ostream &OS,
    const elimination::InterDataFlowResultT<K, FactT, TransferT, NodeT>
        &Result,
    bool &HadSolveError) {
  if (!Result.hasSolveMetadata())
    return;
  recordSolveStatus(Result, HadSolveError);
  OS << "  [solver] status=" << toString(Result.solveStatus()) << "\n";
  if (Result.hasContextSolveDiagnostics()) {
    const auto &D = Result.contextSolveDiagnostics();
    OS << "  [context-summary] pairs=" << D.procedure_context_count
       << ", builds=" << D.procedure_summary_builds
       << ", reuses=" << D.procedure_summary_reuses
       << ", gen_us=" << D.gen_time_us << ", norm_us=" << D.norm_time_us
       << ", interp_us=" << D.interp_time_us
       << ", semantic_star_ns=" << D.semantic_star_time_ns
       << ", star_iters=" << D.star_iterations_total << "\n";
    emitOrderingDiagnostics(OS, D.ordering);
  }
}

template <unsigned K, typename FactT, typename TransferT, typename NodeT>
void emitInterSummaryDiagnostics(
    llvm::raw_ostream &OS,
    const elimination::InterDataFlowResultT<K, FactT, TransferT, NodeT>
        &Result) {
  if (!Result.hasSummarySolveDiagnostics())
    return;
  const auto &D = Result.summarySolveDiagnostics();
  OS << "  [inter-summary] contexts=" << D.discovered_context_node_count
     << ", seeds=" << D.seed_count << ", eqn_nodes=" << D.equation_node_count
     << ", eqn_edges=" << D.equation_edge_count << ", scc=" << D.scc_count
     << ", cyclic_scc=" << D.cyclic_scc_count << ", gen_us=" << D.gen_time_us
     << ", norm_us=" << D.norm_time_us << ", interp_us=" << D.interp_time_us
     << ", semantic_star_ns=" << D.semantic_star_time_ns
     << ", star_iters=" << D.star_iterations_total << "\n";
  emitOrderingDiagnostics(OS, D.ordering);
  auto EmitStats = [&](const char *Tag, const elimination::ean::DagStats &S) {
    OS << "  [" << Tag << "] nodes=" << S.uniqueNodes
       << ", edges=" << S.uniqueEdges << ", tree=" << S.expandedTree
       << ", seq=" << S.concats << ", stars=" << S.stars
       << ", unions=" << S.unions << ", atoms=" << S.atoms
       << ", sharing=" << S.sharing() << "\n";
  };
  EmitStats("dagstats-before", D.summary_before);
  EmitStats("dagstats-after", D.summary_after);
}

template <typename ResultT>
void dumpProfile(llvm::raw_ostream &OS,
                 const lotus::dataflow_tool::FunctionView &View,
                 const ResultT &Result, bool DumpExprs,
                 bool &HadSolveError) {
  const auto CFG = collectCFGStats(View.Function);
  OS << "  [cfg] args=" << CFG.Arguments << ", blocks=" << CFG.Blocks
     << ", insts=" << CFG.Instructions << ", edges=" << CFG.Edges
     << ", branching_blocks=" << CFG.BranchingBlocks
     << ", max_succs=" << CFG.MaxSuccessors << ", phis=" << CFG.PhiNodes
     << ", calls=" << CFG.Calls << ", returns=" << CFG.Returns
     << ", unreachable=" << CFG.Unreachable << "\n";
  printSolveMetadata(OS, Result, HadSolveError);

  using ResultTransferT = typename ResultT::transfer_t;
  using BatchExprRef =
      typename elimination::PathExprFactory<ResultTransferT>::Ref;
  std::vector<BatchExprRef> Batch;
  Batch.reserve(View.OrderedInsts.size());
  for (auto *I : View.OrderedInsts) {
    auto E = Result.ExprTo(I);
    if (E)
      Batch.push_back(E);
  }
  const auto DS = elimination::ean::computeDagStats<ResultTransferT>(Batch);
  OS << "  [dagstats] nodes=" << DS.uniqueNodes << ", edges=" << DS.uniqueEdges
     << ", tree=" << DS.expandedTree << ", seq=" << DS.concats
     << ", stars=" << DS.stars << ", unions=" << DS.unions
     << ", atoms=" << DS.atoms << ", sharing=" << DS.sharing()
     << ", roots=" << Batch.size() << "\n";

  size_t NodesWithExpr = 0;
  size_t MissingExpr = 0;
  size_t TotalUniqueNodes = 0;
  size_t TotalSharedRefs = 0;
  size_t TotalStars = 0;
  size_t TotalUnions = 0;
  size_t TotalConcats = 0;
  size_t MaxExprNodes = 0;
  size_t MaxExprDepth = 0;
  std::string MaxExprInst = "none";
  std::string DeepestExprInst = "none";

  for (auto *I : View.OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    if (!Expr) {
      ++MissingExpr;
      continue;
    }
    ++NodesWithExpr;
    const auto Profile = collectExprProfile(Expr);
    TotalUniqueNodes += Profile.UniqueNodes;
    TotalSharedRefs += Profile.SharedRefs;
    TotalStars += Profile.StarNodes;
    TotalUnions += Profile.UnionNodes;
    TotalConcats += Profile.ConcatNodes;
    if (Profile.UniqueNodes > MaxExprNodes) {
      MaxExprNodes = Profile.UniqueNodes;
      MaxExprInst = View.ValueToId.at(I);
    }
    if (Profile.MaxDepth > MaxExprDepth) {
      MaxExprDepth = Profile.MaxDepth;
      DeepestExprInst = View.ValueToId.at(I);
    }
  }

  OS << "  [expr-profile] with_expr=" << NodesWithExpr
     << ", missing_expr=" << MissingExpr
     << ", total_unique_nodes=" << TotalUniqueNodes
     << ", total_shared_refs=" << TotalSharedRefs
     << ", total_unions=" << TotalUnions << ", total_concats=" << TotalConcats
     << ", total_stars=" << TotalStars << ", max_nodes=" << MaxExprNodes << "@"
     << MaxExprInst << ", max_depth=" << MaxExprDepth << "@" << DeepestExprInst
     << "\n";

  if (!DumpExprs)
    return;

  for (auto *I : View.OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    OS << "  [expr] " << View.ValueToId.at(I);
    if (!Expr) {
      OS << " missing\n";
      continue;
    }
    const auto Profile = collectExprProfile(Expr);
    OS << " nodes=" << Profile.UniqueNodes << ", depth=" << Profile.MaxDepth
       << ", atoms=" << Profile.AtomNodes << ", unions=" << Profile.UnionNodes
       << ", concats=" << Profile.ConcatNodes << ", stars=" << Profile.StarNodes
       << ", shared_refs=" << Profile.SharedRefs << ", expr=";
    formatPathExpr(OS, Expr, View.ValueToId);
    OS << "\n";
  }
}

}

#endif
