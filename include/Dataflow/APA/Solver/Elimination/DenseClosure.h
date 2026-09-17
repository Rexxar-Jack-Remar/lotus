#pragma once

#include "Dataflow/APA/Core/PathExpr.h"

#include <numeric>
#include <vector>

namespace elimination::detail {

template <typename TransferT>
using DenseMatrix =
    std::vector<std::vector<typename PathExprFactory<TransferT>::Ref>>;

// Full-matrix Lehmann closure. Retain all endpoints, snapshotting each pivot's
// row/column before updating. N denotes the initial observer event, not a
// pivot.
template <typename TransferT, typename ObserverT>
void closeDenseMatrix(DenseMatrix<TransferT> &Matrix,
                      const PathExprFactory<TransferT> &Factory,
                      const std::vector<std::size_t> &Order,
                      ObserverT Observe) {
  using Exprs = PathExprFactory<TransferT>;
  const auto N = Matrix.size();
  std::vector<typename Exprs::Ref> Column(N), Row(N);
  Observe(N);
  for (auto Pivot : Order) {
    for (std::size_t I = 0; I < N; ++I)
      Column[I] = Matrix[I][Pivot];
    for (std::size_t J = 0; J < N; ++J)
      Row[J] = Matrix[Pivot][J];
    const auto Loop = Factory.star(Matrix[Pivot][Pivot]);
    for (std::size_t I = 0; I < N; ++I) {
      if (Exprs::isZero(Column[I]))
        continue;
      for (std::size_t J = 0; J < N; ++J) {
        if (Exprs::isZero(Row[J]))
          continue;
        const auto Via =
            Factory.concat(Factory.concat(Column[I], Loop), Row[J]);
        Matrix[I][J] = Factory.unite(Matrix[I][J], Via);
      }
    }
    Observe(Pivot);
  }
}

template <typename TransferT>
void closeDenseMatrix(DenseMatrix<TransferT> &Matrix,
                      const PathExprFactory<TransferT> &Factory) {
  std::vector<std::size_t> Order(Matrix.size());
  std::iota(Order.begin(), Order.end(), 0);
  closeDenseMatrix(Matrix, Factory, Order, [](std::size_t) {});
}

} // namespace elimination::detail
