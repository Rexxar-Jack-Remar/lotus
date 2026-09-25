#pragma once

// CostModel: per-operator weights (paper §III.D) plus the Eq. 5 shared-DAG
// objective weights. This is plain data with NO e-graph/egg dependency, so it
// can be embedded in EliminationOptions (Core) without pulling egg headers into
// Core. The egg-facing cost function that uses these weights is PathCostFn,
// defined in CostFn.h.

namespace elimination {
namespace ean {

struct CostModel {
  double wJoin = 1.0; // ⊕   (per-operator weights w_o = the operator term)
  double wSeq = 1.0;  // ·
  double wStar = 1.0; // *
  double wAtom = 1.0;
  double wZero = 0.0;
  double wOne = 0.0;

  // Eq. 5 shared-DAG objective weights (used by the reuse-aware extractor):
  //   C_DAG = alpha*N_unique + beta*E_unique + sum_o w_o*N_o + gamma*C_repeat.
  double alpha = 1.0; // unique DAG nodes (retained memory)
  double beta = 0.0;  // unique DAG edges (factory construction)
  double gamma = 0.0; // repeated work under a non-memoizing interpreter

  // Uniform structural weights (approximates unique-node count).
  static CostModel uniform() { return CostModel{}; }

  // A composition/closure-heavy profile: star > seq > join.
  static CostModel profiled() {
    CostModel m;
    m.wStar = 4.0;
    m.wSeq = 2.0;
    m.wJoin = 1.0;
    m.wAtom = 1.0;
    m.wZero = 0.0;
    m.wOne = 0.0;
    return m;
  }

  // Shared-DAG objective (Eq. 5) that scores by unique nodes AND edges, so the
  // reuse-aware extractor minimizes the *exported* factory node count (which
  // tracks variadic-child edges after re-binarization) rather than just the
  // e-class count. Candidate generation is unchanged (per-op weight 1); only the
  // scoring changes, biasing selection toward the fewest-edge equivalent form.
  static CostModel dag() {
    CostModel m;
    m.wJoin = m.wSeq = m.wStar = m.wAtom = 1.0;
    m.wZero = m.wOne = 0.0;
    m.alpha = 1.0; // unique DAG nodes
    m.beta = 1.0;  // unique DAG edges (≈ exported factory nodes)
    m.gamma = 0.0; // retained-size objective, not interpreter repeat cost
    return m;
  }
};

} // namespace ean
} // namespace elimination

