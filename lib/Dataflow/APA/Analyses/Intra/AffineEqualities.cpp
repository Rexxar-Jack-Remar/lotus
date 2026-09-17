#include "Dataflow/APA/Analyses/Intra/AffineEqualities.h"

#include "Dataflow/APA/Solver/Intra/IntraSolver.h"

#include <chrono>
#include <unordered_map>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

namespace elimination {
namespace {
using D = AffineRelationDomain;
struct AffineIntraTypes {
  using n_t = llvm::Instruction *;
  using fact_t = AffineFact;
  using transfer_t = AffineEdgeTransfer;
  using abstract_domain_t = D;
};

// Reuse LLVM's existing graph/dominator adapter; only transfers carry edges.
class AffineGraph : public LLVMIntraEliminationProblem<AffineFact, D> {
public:
  using LLVMIntraEliminationProblem::LLVMIntraEliminationProblem;
  AffineFact initialFact() const override { return D::one(); }
  AffineFact applyTransfer(const transfer_t &,
                           const AffineFact &in) const override {
    return in;
  }
};

class AffineProblem
    : public IntraReducibleEliminationProblem<AffineIntraTypes> {
public:
  explicit AffineProblem(llvm::Function *function) : graph(function) {}
  std::vector<n_t> nodes() const override { return graph.nodes(); }
  n_t entry() const override { return graph.entry(); }
  std::vector<n_t> succs(n_t node) const override { return graph.succs(node); }
  std::vector<Edge> edges() const override {
    std::vector<Edge> result;
    for (const auto &edge : graph.edges())
      result.push_back({edge.Src, edge.Dst});
    return result;
  }
  std::vector<n_t> topologicalOrder() const override {
    return graph.topologicalOrder();
  }
  n_t idom(n_t node) const override { return graph.idom(node); }
  bool dominates(n_t a, n_t b) const override { return graph.dominates(a, b); }
  transfer_t edgeTransfer(n_t src, n_t dst) const override {
    return {src, dst};
  }
  fact_t initialFact() const override { return D::one(); }
  fact_t applyTransfer(const transfer_t &edge,
                       const fact_t &in) const override {
    if (!edge.inst)
      return in;
    const auto key = std::make_pair(edge.inst, edge.succ);
    auto found = transfers.find(key);
    if (found != transfers.end())
      return apply(found->second, in);
    std::vector<AffineFact> sequence;
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(edge.inst)) {
      if (builder.isAssumeLikeCall(*call))
        sequence.push_back(builder.instructionTransfer(*call));
      else if (D::isTrackedValue(call))
        sequence.push_back(D::makeForget(call));
    } else if (!llvm::isa<llvm::PHINode>(edge.inst) &&
               builder.instructionHasEffect(*edge.inst)) {
      sequence.push_back(builder.instructionTransfer(*edge.inst));
    }
    if (edge.succ && edge.inst->isTerminator()) {
      if (builder.edgeHasCondition(*edge.inst))
        sequence.push_back(
            builder.edgeTransferRelation(*edge.inst, *edge.succ));
      if (builder.edgeEntersPhi(*edge.inst, *edge.succ))
        sequence.push_back(builder.phiTransferForEdge(*edge.inst, *edge.succ));
    }
    auto result = apply(sequence, in);
    transfers.emplace(key, std::move(sequence));
    return result;
  }

private:
  static fact_t apply(const std::vector<AffineFact> &sequence, fact_t in) {
    for (const auto &transfer : sequence)
      in = D::extend(transfer, in);
    return in;
  }
  AffineGraph graph;
  AffineTransferBuilder builder;
  mutable std::map<std::pair<n_t, n_t>, std::vector<AffineFact>> transfers;
};

using Factory = PathExprFactory<AffineEdgeTransfer>;

// Memoize evaluation, not context-independent summaries: a guard following a
// join must see the joined input just as in the ordinary interpreter.
class AffineInterpreter {
public:
  AffineInterpreter(const AffineProblem &problem,
                    const EliminationOptions &options,
                    SolveDiagnostics &diagnostics)
      : problem(problem), options(options), diagnostics(diagnostics) {}
  AffineFact eval(const Factory::Ref &expr, const AffineFact &in) {
    if (!expr || (failed &&
                  options.NonConvergentStarPolicy == OnNonConvergentStar::Fail))
      return D::zero();
    if (options.InterpMemo) {
      auto found = memo.find(expr.get());
      if (found != memo.end())
        for (const auto &entry : found->second)
          if (D::equal(entry.first, in))
            return entry.second;
    }
    auto result = evalFresh(expr, in);
    if (options.InterpMemo && !failed && entries < 4096) {
      memo[expr.get()].emplace_back(in, result);
      ++entries;
    }
    return result;
  }

private:
  AffineFact evalFresh(const Factory::Ref &expr, const AffineFact &in) {
    switch (expr->K) {
    case Factory::Kind::Zero:
      return D::zero();
    case Factory::Kind::One:
      return in;
    case Factory::Kind::Atom:
      return problem.applyTransfer(*expr->Transfer, in);
    case Factory::Kind::Union: {
      auto lhs = eval(expr->L, in);
      auto rhs = eval(expr->R, in);
      return D::combine(lhs, rhs);
    }
    case Factory::Kind::Concat: {
      auto mid = eval(expr->L, in);
      return eval(expr->R, mid);
    }
    case Factory::Kind::Star: {
      detail::ScopedNestedNanoseconds timer(diagnostics.semantic_star_time_ns, star_depth);
      auto current = in;
      const auto limit = options.MaxStarIterations
                             ? options.MaxStarIterations
                             : problem.maxStarIterations();
      for (std::size_t i = 0; i < limit; ++i) {
        ++diagnostics.star_iterations_total;
        auto next = D::combine(in, eval(expr->L, current));
        if (D::equal(next, current))
          return current;
        current = std::move(next);
      }
      diagnostics.max_star_hit = true;
      failed = true;
      // Match legacy payload policies, but always report NonConvergentStar.
      return options.NonConvergentStarPolicy == OnNonConvergentStar::ReturnLast
                 ? current
                 : D::zero();
    }
    }
    return D::zero();
  }
  const AffineProblem &problem;
  const EliminationOptions &options;
  SolveDiagnostics &diagnostics;
  std::size_t star_depth = 0;
  bool failed = false;
  std::size_t entries = 0;
  std::unordered_map<const Factory::Expr *,
                     std::vector<std::pair<AffineFact, AffineFact>>>
      memo;
};
} // namespace

AffineEqualitiesResult
runIntraElimAffineEqualities(llvm::Function *function,
                             EliminationOptions options) {
  AffineEqualitiesResult result;
  if (!function || function->isDeclaration())
    return result;
  auto add = [&](const llvm::Value *value) {
    if (!AffineTransferBuilder::isTrackedScalar(value))
      return;
    result.vocabulary.indices[value] = result.vocabulary.values.size();
    result.vocabulary.actualBitWidths[value] =
        value->getType()->getIntegerBitWidth();
    result.vocabulary.values.push_back(value);
  };
  for (const auto &arg : function->args())
    add(&arg);
  for (const auto &inst : llvm::instructions(*function))
    add(&inst);
  D::configure(&result.vocabulary);
  AffineProblem problem(function);
  auto solver_options = options;
  // Only prefix factoring is valid for arbitrary guarded transfer functions.
  solver_options.EANLaws = options.EANLaws.has(ean::Law::LeftDistributive)
                               ? ean::LawProfile::safeMinimal()
                               : ean::LawProfile::none();
  solver_options.InterpMemo = true;
  IntraEliminationSolver<AffineIntraTypes> solver(problem, solver_options);
  auto status = solver.solve();
  static_cast<
      DataFlowResultT<llvm::Instruction *, AffineFact, AffineEdgeTransfer> &>(
      result) = solver.getResults();
  auto diagnostics = solver.getDiagnostics();
  if (options.EnableEAN) {
    for (unsigned i = 0; i < static_cast<unsigned>(ean::Law::Count); ++i) {
      auto law = static_cast<ean::Law>(i);
      diagnostics.ean_laws_restricted |=
          options.EANLaws.has(law) && !solver_options.EANLaws.has(law);
    }
  }
  if (status != SolveStatus::InvalidProblem) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t repeat = 0;
         repeat < std::max(std::size_t(1), options.InterpRepeat); ++repeat) {
      AffineInterpreter interpreter(problem, options, diagnostics);
      for (auto *node : problem.nodes())
        result.IN(node) = interpreter.eval(result.ExprTo(node), D::one());
    }
    diagnostics.interp_time_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    if (diagnostics.max_star_hit)
      status = SolveStatus::NonConvergentStar;
  }
  result.setSolveMetadata(status, diagnostics);
  return result;
}
} // namespace elimination
