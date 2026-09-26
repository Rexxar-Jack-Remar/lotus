#pragma once

#include "Dataflow/APA/EAN/EAN.h"
#include "Dataflow/APA/EAN/Greedy.h"

#include <chrono>

namespace elimination::detail {

template <typename ContextT> void applyPostPass(ContextT &Context) {
  if (!Context.Opts.EnableEAN && !Context.Opts.EnableGreedy)
    return;
  using Transfer = typename ContextT::transfer_t;
  std::vector<typename ContextT::n_t> Nodes;
  std::vector<typename ContextT::expr_ref_t> Roots;
  const auto &Results = std::as_const(Context.Results);
  for (auto Node : Context.Problem.nodes()) {
    auto Root = Results.ExprTo(Node);
    if (Root) {
      Nodes.push_back(Node);
      Roots.push_back(std::move(Root));
    }
  }
  if (Roots.empty())
    return;
  const auto Start = std::chrono::steady_clock::now();
  auto Optimized = Roots;
  if (Context.Opts.EnableEAN) {
    auto Extract = Context.Opts.EANExtract;
    Extract.gateMinNodes = Context.Opts.EANMinNodes;
    Extract.monotoneGuard = Context.Opts.EANMonotone;
    Optimized = ean::ean<Transfer>(Roots, Context.Opts.EANLaws,
                                   Context.Opts.EANCost, Context.Opts.EANBudget,
                                   Context.Exprs, nullptr, Extract);
  } else {
    Optimized = greedySimplify<Transfer>(Roots, Context.Exprs);
  }
  Context.Diagnostics.norm_time_us +=
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - Start)
          .count();
  const auto InterpStart = std::chrono::steady_clock::now();
  for (std::size_t I = 0; I < Nodes.size(); ++I) {
    Context.Results.ExprTo(Nodes[I]) = Optimized[I];
    if (Context.Opts.InterpMemo)
      continue;
    auto Value =
        Context.Interpreter.eval(Optimized[I], Context.Problem.initialFact());
    for (std::size_t R = 1;
         R < std::max(std::size_t(1), Context.Opts.InterpRepeat); ++R) {
      Value =
          Context.Interpreter.eval(Optimized[I], Context.Problem.initialFact());
    }
    Context.Results.IN(Nodes[I]) = std::move(Value);
  }
  Context.Diagnostics.interp_time_us +=
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - InterpStart)
          .count();
}

} // namespace elimination::detail
