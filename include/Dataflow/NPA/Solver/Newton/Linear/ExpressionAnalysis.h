#pragma once

/**
 * \file
 * \brief Detects whether a linearized expression has LCFL structure.
 *
 * An LCFL equation system has the form Y_j = c_j ⊕ ⊕_{i,k} (a_{i,j,k} ⊗ Y_i ⊗
 * b_{i,j,k}) (Reps et al. TOPLAS 2016, Defn. 3.1): coefficients on both sides
 * of variables. In our AST this corresponds to Concat (a·X·b) and Star
 * (Kleene star over a variable). When present, the tensor-product strategy
 * can regularize the system; otherwise we use worklist/SCC only.
 */

#include "Dataflow/NPA/Core/Expr/Expressions.h"

#include <unordered_set>

namespace npa {

template <class D> struct LCFLDetector {
  /// True if e contains Concat or Star (two-sided or starred structure).
  static bool has_lcfl_structure(const E1<D> &e) {
    std::unordered_set<const Exp1<D> *> visited;
    return has_lcfl_structure(e, visited);
  }

private:
  static bool has_lcfl_structure(
      const E1<D> &e, std::unordered_set<const Exp1<D> *> &visited) {
    if (!e)
      return false;
    if (!visited.insert(e.get()).second)
      return false;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Concat:
    case K::Star:
      return true;
    default:
      break;
    }
    if (e->t && has_lcfl_structure(e->t, visited))
      return true;
    if (e->t1 && has_lcfl_structure(e->t1, visited))
      return true;
    if (e->t2 && has_lcfl_structure(e->t2, visited))
      return true;
    return false;
  }
};

} // namespace npa

