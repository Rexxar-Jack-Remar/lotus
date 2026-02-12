#ifndef NPA_TENSOR_LINEAR_SOLVE_H
#define NPA_TENSOR_LINEAR_SOLVE_H

/**
 * \file
 * \brief Tensor-product linear solver (Reps et al. TOPLAS 2016, Alg. 3.4).
 *
 * Converts the LCFL linear system into a \e left-linear system over the
 * paired semiring (TensorProductDomain): pair (a,b) represents left/right
 * context so that a·Y·b becomes Y ⊗_p (a,b). The left-linear system is
 * solved by worklist (or could use Tarjan path expressions); then we
 * \e project back to the base domain (readout R((w1,w2)) = w1⊗w2).
 * Regularization requires Concat coefficients to be constant; otherwise we
 * fall back to worklist.
 *
 * The implementation uses an exact correlated representation in tensor space
 * for idempotent domains. This avoids correlation loss at projection time, at
 * the cost of potentially larger intermediate values.
 */

#include "Dataflow/NPA/Core/LinearSolvers.h"
#include "Dataflow/NPA/Domains/TensorProductDomain.h"

namespace npa {

/// Constant evaluator for Exp1: succeeds only if expression is variable-free.
template <class D>
struct Exp1ConstEval {
  using V = DomVal<D>;
  using E = E1<D>;
  static Optional<V> eval(const E &e) {
    if (!e) return {};
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Term:
      {
        Optional<V> out;
        out = e->c;
        return out;
      }
    case K::Seq: {
      auto t = eval(e->t);
      if (!t.has_value()) return {};
      Optional<V> out;
      out = D::extend(e->c, *t);
      return out;
    }
    case K::SeqR: {
      auto t = eval(e->t);
      if (!t.has_value()) return {};
      Optional<V> out;
      out = D::extend(*t, e->c);
      return out;
    }
    case K::Add: {
      auto a = eval(e->t1), b = eval(e->t2);
      if (!a.has_value() || !b.has_value()) return {};
      Optional<V> out;
      out = D::combine(*a, *b);
      return out;
    }
    case K::Sub: {
      if (!DomainHasSubtract<D>::value) return {};
      auto a = eval(e->t1), b = eval(e->t2);
      if (!a.has_value() || !b.has_value()) return {};
      Optional<V> out;
      out = D::subtract(*a, *b);
      return out;
    }
    case K::Ndet: {
      auto a = eval(e->t1), b = eval(e->t2);
      if (!a.has_value() || !b.has_value()) return {};
      Optional<V> out;
      out = D::ndetCombine(*a, *b);
      return out;
    }
    case K::Cond: {
      auto t = eval(e->t1), f = eval(e->t2);
      if (!t.has_value() || !f.has_value()) return {};
      Optional<V> out;
      out = D::condCombine(e->phi, *t, *f);
      return out;
    }
    default:
      return {};
    }
  }
};

/// Converts linearized expression over D to a left-linear expression over
/// TensorProductDomain<D> by rewriting Concat (a·X·b) into X ⊗_p (a,b).
template <class D>
struct Exp1ToTensor {
  using TD = TensorProductExactDomain<D>;
  using E1D = E1<D>;
  using E1T = E1<TD>;
  using VT = typename TD::pair_type;
  static bool is_regularizable(const E1D &e) {
    if (!e) return true;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::InfClos:
      // Allow InfClos when its body is regularizable. This keeps tensor
      // regularization applicable to linear systems that use Kleene-star-like
      // closures in the bound variable.
      return is_regularizable(e->t);
    case K::Concat: {
      auto a = Exp1ConstEval<D>::eval(e->t1);
      auto b = Exp1ConstEval<D>::eval(e->t2);
      return a.has_value() && b.has_value();
    }
    default:
      break;
    }
    if (e->t && !is_regularizable(e->t)) return false;
    if (e->t1 && !is_regularizable(e->t1)) return false;
    if (e->t2 && !is_regularizable(e->t2)) return false;
    return true;
  }
  static E1T convert(const E1D &e) {
    if (!e) return nullptr;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Term:
      return Exp1<TD>::term(TD::singleton(e->c, D::one()));
    case K::Seq:
      // Base: c ⊗ t. Use seqR so projection yields c ⊗ R(t).
      return Exp1<TD>::seqR(convert(e->t), TD::singleton(e->c, D::one()));
    case K::SeqR:
      // Base: t ⊗ c. Use seq so projection yields R(t) ⊗ c.
      return Exp1<TD>::seq(TD::singleton(e->c, D::one()), convert(e->t));
    case K::Call:
      // Base: f ⊗ c. Encode as seq so projection yields R(f) ⊗ c.
      return Exp1<TD>::seq(TD::singleton(e->c, D::one()), Exp1<TD>::hole(e->sym));
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
      // Regularization: a·X·b -> X ⊗_p (a,b) (TOPLAS 2016, Alg. 3.4).
      {
        auto a = Exp1ConstEval<D>::eval(e->t1);
        auto b = Exp1ConstEval<D>::eval(e->t2);
        return Exp1<TD>::seqR(Exp1<TD>::hole(e->sym), TD::singleton(*a, *b));
      }
    case K::InfClos:
      return Exp1<TD>::inf(convert(e->t), e->sym);
    default:
      return nullptr;
    }
  }
};

/// Solve LCFL linear system by lifting to tensor space: convert RHS to
/// TensorProductDomain, solve (left-linear over pairs), project back via R.
template <class D>
std::vector<DomVal<D>> solve_linear_tensor_impl(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    std::vector<DomVal<D>> init) {
  if (!D::idempotent) {
    if (verbose)
      std::cerr << "[tensor] exact tensor mode requires idempotent domain; falling back to worklist\n";
    return solve_linear_worklist_impl<D>(verbose, rhs, init);
  }
  for (const auto &p : rhs) {
    if (!Exp1ToTensor<D>::is_regularizable(p.second)) {
      if (verbose)
        std::cerr << "[tensor] not regularizable; falling back to worklist\n";
      return solve_linear_worklist_impl<D>(verbose, rhs, init);
    }
  }
  using TD = TensorProductExactDomain<D>;
  using VT = typename TD::value_type;
  std::vector<std::pair<Symbol, E1<TD>>> rhs_tensor;
  rhs_tensor.reserve(rhs.size());
  for (const auto &p : rhs)
    rhs_tensor.emplace_back(p.first, Exp1ToTensor<D>::convert(p.second));
  std::vector<VT> init_tensor;
  init_tensor.reserve(init.size());
  for (const auto &v : init) init_tensor.emplace_back(TD::singleton(v, v));
  std::vector<VT> delta_tensor =
      solve_linear_worklist_impl<TD>(verbose, rhs_tensor, init_tensor);
  std::vector<DomVal<D>> delta;
  delta.reserve(delta_tensor.size());
  for (const auto &p : delta_tensor) delta.push_back(TD::project(p));
  return delta;
}

} // namespace npa

#endif // NPA_TENSOR_LINEAR_SOLVE_H
