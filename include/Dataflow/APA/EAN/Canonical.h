#pragma once

// Canonical e-node builders shared by Import (raw DAG -> e-graph) and Factorize
// (rewrite passes). These enforce the profile-relative canonical form (paper
// invariant I2) at the point of construction:
//   - join: canonicalize children to reps, drop 0, sort, dedup (ACI);
//           empty -> 0, singleton -> the element itself.
//   - seq:  canonicalize children, drop 1, annihilate on 0, keep order (no
//           dedup); empty -> 1, singleton -> the element itself.
//   - star: (A*)* = A*, 0* = 1* = 1.
//
// These are the universally-valid Kleene simplifications (the same ones the
// baseline PathExprFactory applies), so they are unconditional. Exploratory,
// profile-gated rewrites live elsewhere (see Factorize.h, LawProfile.h).

#include <algorithm>
#include <utility>
#include <vector>

#include "Dataflow/APA/EAN/PathLang.h"
#include "Solvers/EGraph/Analysis.h"
#include "Solvers/EGraph/EGraph.h"

namespace elimination {
namespace ean {

// The concrete e-graph type EAN builds. No e-class analysis data is needed for
// import, canonicalization, or factorization.
using Graph = ::lotus::egraph::EGraph<PathLang, ::lotus::egraph::NoAnalysis<PathLang>>;

namespace canon {

inline Id zeroId(Graph &g) { return g.add(makeZero()); }
inline Id oneId(Graph &g) { return g.add(makeOne()); }

// Build a canonical JOIN (⊕) e-class from `members`.
inline Id join(Graph &g, std::vector<Id> members) {
  const Id zero = zeroId(g);
  std::vector<Id> kept;
  kept.reserve(members.size());
  for (Id m : members) {
    m = g.find(m);
    if (m != zero) {
      kept.push_back(m);
    }
  }
  std::sort(kept.begin(), kept.end());
  kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
  if (kept.empty()) {
    return zero;
  }
  if (kept.size() == 1) {
    return kept.front();
  }
  return g.add(makeJoin(std::move(kept)));
}

// Build a canonical SEQ (·) e-class from `members` (order preserved).
inline Id seq(Graph &g, std::vector<Id> members) {
  const Id zero = zeroId(g);
  const Id one = oneId(g);
  std::vector<Id> kept;
  kept.reserve(members.size());
  for (Id m : members) {
    m = g.find(m);
    if (m == zero) {
      return zero; // 0 · x = x · 0 = 0
    }
    if (m == one) {
      continue; // 1 · x = x · 1 = x
    }
    kept.push_back(m);
  }
  if (kept.empty()) {
    return one;
  }
  if (kept.size() == 1) {
    return kept.front();
  }
  return g.add(makeSeq(std::move(kept)));
}

// Build a canonical STAR (*) e-class from body `sub`.
inline Id star(Graph &g, Id sub) {
  sub = g.find(sub);
  const PathLang &n = g[sub].nodes.front();
  if (isStar(n)) {
    return sub; // (A*)* = A*
  }
  if (isZero(n) || isOne(n)) {
    return oneId(g); // 0* = 1* = 1
  }
  return g.add(makeStar(sub));
}

} // namespace canon
} // namespace ean
} // namespace elimination

