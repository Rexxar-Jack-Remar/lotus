#ifndef NPA_LCFL_DETECTOR_H
#define NPA_LCFL_DETECTOR_H

#include "Dataflow/NPA/Core/Expressions.h"

namespace npa {

template <class D>
struct LCFLDetector {
  static bool has_lcfl_structure(const E1<D> &e) {
    if (!e) return false;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Concat:
    case K::InfClos:
      return true;
    default:
      break;
    }
    if (e->t && has_lcfl_structure(e->t)) return true;
    if (e->t1 && has_lcfl_structure(e->t1)) return true;
    if (e->t2 && has_lcfl_structure(e->t2)) return true;
    return false;
  }
};

} // namespace npa

#endif // NPA_LCFL_DETECTOR_H
