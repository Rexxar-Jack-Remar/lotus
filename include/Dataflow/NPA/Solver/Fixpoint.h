#ifndef NPA_FIXPOINT_H
#define NPA_FIXPOINT_H

/**
 * \file
 * \brief Generic fixpoint/iteration primitives used by NPA internals.
 *
 * This header is intentionally lower-level than the public solver entry points:
 *
 * This is a low-level Core utility, not a high-level equation-system solver.
 * It provides reusable least-fixpoint iteration for:
 * - local sub-problems such as `Star` / `Mu` during evaluation
 * - synchronized iteration over already-linearized systems
 */

#include "Dataflow/NPA/Core/Domain.h"
#include "Dataflow/NPA/Solver/SolveContext.h"

#include <iostream>

namespace npa {

/// Single-variable fixpoint: iterates until stable (`x_{i+1} = f(x_i)`).
template <class D, class F> auto fix(bool verbose, DomVal<D> init, F f) {
  NPA_REQUIRE_DOMAIN(D);
  int cnt = 0;
  auto last = init;
  const int max_iters = domain_max_fixpoint_iters<D>();
  while (true) {
    auto nxt = f(last);
    if (domain_equal<D>(last, nxt)) {
      if (verbose)
        std::cerr << "[fp] " << cnt + 1 << "\n";
      return nxt;
    }
    last = std::move(nxt);
    ++cnt;
    if (max_iters >= 0 && cnt >= max_iters) {
      npa_note_fixpoint_limit_hit();
      if (verbose)
        std::cerr << "[fp] hit max_fixpoint_iters=" << max_iters << "\n";
      return last;
    }
  }
}

/// Vector fixpoint for synchronized iteration over a tuple of variables.
template <class D, class Vec, class F>
Vec fix_vec(bool verbose, Vec init, F f) {
  int cnt = 0;
  const int max_iters = domain_max_fixpoint_iters<D>();
  while (true) {
    Vec nxt = f(init);
    bool stable = true;
    for (size_t i = 0; i < init.size(); ++i) {
      if (!domain_equal<D>(init[i], nxt[i])) {
        stable = false;
        break;
      }
    }
    if (stable) {
      if (verbose)
        std::cerr << "[fp] " << cnt + 1 << "\n";
      return nxt;
    }
    init.swap(nxt);
    ++cnt;
    if (max_iters >= 0 && cnt >= max_iters) {
      npa_note_fixpoint_limit_hit();
      if (verbose)
        std::cerr << "[fp] hit max_fixpoint_iters=" << max_iters << "\n";
      return init;
    }
  }
}

} // namespace npa

#endif // NPA_FIXPOINT_H
