#pragma once

#include "Dataflow/APA/Core/PathExpr.h"

#include <unordered_set>
#include <utility>

namespace elimination::detail {

template <typename TransferT>
std::pair<std::size_t, std::size_t>
countDag(const std::vector<typename PathExprFactory<TransferT>::Ref> &Roots) {
  using Factory = PathExprFactory<TransferT>;
  std::vector<const typename Factory::Expr *> Pending;
  for (const auto &Root : Roots) {
    if (Root && !Factory::isZero(Root))
      Pending.push_back(Root.get());
  }
  std::unordered_set<const typename Factory::Expr *> Seen;
  std::size_t Edges = 0;
  while (!Pending.empty()) {
    const auto *Node = Pending.back();
    Pending.pop_back();
    if (!Seen.insert(Node).second)
      continue;
    for (const auto &Child : {Node->L, Node->R}) {
      if (Child) {
        ++Edges;
        Pending.push_back(Child.get());
      }
    }
  }
  return {Seen.size(), Edges};
}

} // namespace elimination::detail
