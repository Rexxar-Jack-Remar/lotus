#pragma once

#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Solver/Ordering/Policy.h"

#include <map>

namespace elimination::order {

// Read-only local graph view. Metadata must have been cached at construction;
// scoring never traverses expressions or executes speculative elimination.
template <typename TransferT, typename EdgeExistsT>
OrderSignals collectSignals(
    std::size_t Node,
    const std::map<std::size_t, typename PathExprFactory<TransferT>::Ref> &Pred,
    const std::map<std::size_t, typename PathExprFactory<TransferT>::Ref> &Succ,
    const typename PathExprFactory<TransferT>::Ref &Self,
    const PathExprFactory<TransferT> &Factory,
    const OrderPolicyOptions &Options, PolicyRequirements Needs,
    EdgeExistsT EdgeExists) {
  using Exprs = PathExprFactory<TransferT>;
  const auto HasSelf = static_cast<std::size_t>(static_cast<bool>(Self));
  OrderSignals Signals;
  Signals.structural = static_cast<double>(Pred.size() - HasSelf) *
                       static_cast<double>(Succ.size() - HasSelf);
  auto Size = [&](const typename Exprs::Ref &Root) {
    const auto Count =
        Factory.cachedReachableNodeCount(Root, Options.DAGSizeCap);
    if (!Count)
      throw std::logic_error(
          "APA scoring requires construction-cached DAG sizes");
    return *Count;
  };
  const bool Nontrivial = Self && !Exprs::isZero(Self) && !Exprs::isOne(Self);
  const auto SelfSize =
      Nontrivial && (Needs.expression_growth || Needs.star_exposure)
          ? Size(Self)
          : 0;
  if (Needs.expression_growth || Needs.new_fill) {
    for (const auto &P : Pred) {
      if (P.first == Node)
        continue;
      for (const auto &S : Succ) {
        if (S.first == Node)
          continue;
        if (Needs.expression_growth) {
          Signals.expression += static_cast<double>(Size(P.second)) +
                                static_cast<double>(SelfSize) +
                                static_cast<double>(Size(S.second)) + 2.0;
        }
        if (Needs.new_fill && P.first != S.first &&
            !EdgeExists(P.first, S.first)) {
          Signals.fill += 1.0;
        }
      }
    }
  }
  if (Needs.star_exposure && Nontrivial &&
      (!Options.IsStarResultCached ||
       !Options.IsStarResultCached(Self.get()))) {
    Signals.star = static_cast<double>(SelfSize);
  }
  return Signals;
}

} // namespace elimination::order
