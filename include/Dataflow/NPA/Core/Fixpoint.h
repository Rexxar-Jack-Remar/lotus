#ifndef NPA_FIXPOINT_H
#define NPA_FIXPOINT_H

#include "Dataflow/NPA/Core/NPACommon.h"

namespace npa {

template <class D, class F>
auto fix(bool verbose, DomVal<D> init, F f) {
  NPA_REQUIRE_DOMAIN(D);
  int cnt = 0;
  auto last = init;
  while (true) {
    auto nxt = f(last);
    if (D::equal(last, nxt)) {
      if (verbose) std::cerr << "[fp] " << cnt + 1 << "\n";
      return nxt;
    }
    last = std::move(nxt);
    ++cnt;
  }
}

template <class D, class Vec, class F>
Vec fix_vec(bool verbose, Vec init, F f) {
  int cnt = 0;
  while (true) {
    Vec nxt = f(init);
    bool stable = true;
    for (size_t i = 0; i < init.size(); ++i) {
      if (!D::equal(init[i], nxt[i])) {
        stable = false;
        break;
      }
    }
    if (stable) {
      if (verbose) std::cerr << "[fp] " << cnt + 1 << "\n";
      return nxt;
    }
    init.swap(nxt);
    ++cnt;
  }
}

} // namespace npa

#endif // NPA_FIXPOINT_H
