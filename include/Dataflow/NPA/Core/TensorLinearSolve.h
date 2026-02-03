#ifndef NPA_TENSOR_LINEAR_SOLVE_H
#define NPA_TENSOR_LINEAR_SOLVE_H

#include "Dataflow/NPA/Core/LinearSolvers.h"
#include "Dataflow/NPA/Domains/TensorProductDomain.h"

namespace npa {

template <class D>
struct Exp1ToTensor {
  using TD = TensorProductDomain<D>;
  using E1D = E1<D>;
  using E1T = E1<TD>;
  static E1T convert(const E1D &e) {
    if (!e) return nullptr;
    using K = typename Exp1<D>::K;
    using VT = typename TD::value_type;
    switch (e->k) {
    case K::Term:
      return Exp1<TD>::term(VT(e->c, e->c));
    case K::Seq:
      return Exp1<TD>::seq(VT(e->c, e->c), convert(e->t));
    case K::Call:
      return Exp1<TD>::call(e->sym, VT(e->c, e->c));
    case K::Cond:
      return Exp1<TD>::cond(e->phi, convert(e->t1), convert(e->t2));
    case K::Add:
      return Exp1<TD>::add(convert(e->t1), convert(e->t2));
    case K::Sub:
      return Exp1<TD>::sub(convert(e->t1), convert(e->t2));
    case K::Ndet:
      return Exp1<TD>::ndet(convert(e->t1), convert(e->t2));
    case K::Hole:
      return Exp1<TD>::hole(e->sym);
    case K::Concat:
      return Exp1<TD>::concat(convert(e->t1), e->sym, convert(e->t2));
    case K::InfClos:
      return Exp1<TD>::inf(convert(e->t), e->sym);
    default:
      return nullptr;
    }
  }
};

template <class D>
std::vector<DomVal<D>> solve_linear_tensor_impl(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    std::vector<DomVal<D>> init) {
  using TD = TensorProductDomain<D>;
  using VT = typename TD::value_type;
  std::vector<std::pair<Symbol, E1<TD>>> rhs_tensor;
  rhs_tensor.reserve(rhs.size());
  for (const auto &p : rhs)
    rhs_tensor.emplace_back(p.first, Exp1ToTensor<D>::convert(p.second));
  std::vector<VT> init_tensor;
  init_tensor.reserve(init.size());
  for (const auto &v : init) init_tensor.emplace_back(v, v);
  std::vector<VT> delta_tensor =
      solve_linear_worklist_impl<TD>(verbose, rhs_tensor, init_tensor);
  std::vector<DomVal<D>> delta;
  delta.reserve(delta_tensor.size());
  for (const auto &p : delta_tensor) delta.push_back(TD::project(p));
  return delta;
}

} // namespace npa

#endif // NPA_TENSOR_LINEAR_SOLVE_H
