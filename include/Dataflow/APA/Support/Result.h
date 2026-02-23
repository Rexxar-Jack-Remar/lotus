#ifndef DATAFLOW_ELIMINATION_SUPPORT_RESULT_H_
#define DATAFLOW_ELIMINATION_SUPPORT_RESULT_H_

#include "Dataflow/APA/Core/PathExpression.h"

#include <unordered_map>

namespace elimination {

template <typename NodeT, typename FactT, typename TransferT>
class DataFlowResultT {
public:
  using expr_factory_t = PathExprFactory<TransferT>;
  using expr_ref_t = typename expr_factory_t::Ref;

  FactT &IN(const NodeT &N) { return In[N]; }
  expr_ref_t &ExprTo(const NodeT &N) { return Expr[N]; }

  const FactT &IN(const NodeT &N) const {
    auto It = In.find(N);
    if (It != In.end()) {
      return It->second;
    }
    return DefaultFact;
  }

  expr_ref_t ExprTo(const NodeT &N) const {
    auto It = Expr.find(N);
    if (It != Expr.end()) {
      return It->second;
    }
    return {};
  }

private:
  std::unordered_map<NodeT, FactT> In;
  std::unordered_map<NodeT, expr_ref_t> Expr;
  FactT DefaultFact{};
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_SUPPORT_RESULT_H_
