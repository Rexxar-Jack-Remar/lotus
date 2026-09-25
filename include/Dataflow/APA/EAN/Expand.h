#pragma once

// Expand: guarded expansion — the Explore-phase primitive (paper §III.C,
// Table III). It is the REVERSE of factorization: distribute a JOIN that sits
// inside a SEQ so a shared sub-product can be exposed and reused,
//
//     P·(r1 ⊕ … ⊕ rk)·S   =   (P·r1·S) ⊕ … ⊕ (P·rk·S)
//
// added as an equivalent representation of the SEQ's e-class (the original is
// retained; extraction chooses). Because expansion is the inverse of the Factor
// phase it can, unguarded, undo factorization or explode the e-graph. It is
// therefore admitted only under an OPPORTUNITY GUARD: at least
// `expandMinAligned` of the produced branches P·rm·S must ALREADY exist as
// e-classes (i.e. the expansion aligns with structure already present, so it
// exposes real sharing rather than inventing new terms), and the expansion adds
// at most `expandGrowthCap` new e-nodes. Expansion at a trailing JOIN (P·(⊕r))
// uses left distributivity; at a leading JOIN ((⊕r)·S) it uses right
// distributivity; a JOIN in the middle needs both — the same law gating as
// factorization, so soundness is inherited.
//
// Termination: both forms are retained and hash-consed, so re-expanding an
// existing form is a uniteChecked no-op, and a Factor/Explore alternation
// converges (the reversed form already exists); the round/e-node budget is the
// backstop. Two phases (scan read-only, then materialize) mirror Factorize.

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "Dataflow/APA/EAN/Canonical.h"
#include "Dataflow/APA/EAN/ExtractOptions.h"
#include "Dataflow/APA/EAN/LawProfile.h"
#include "Dataflow/APA/EAN/PathLang.h"

namespace elimination {
namespace ean {
namespace detail {

// Append `id`'s SEQ elements (flattened one level) to `out`, or `id` itself when
// its class is not a sequence. Mirrors the flattened form the original
// (pre-factorization) sequence had, so a reversed factorization aligns with it.
inline void appendClassElems(Graph &g, Id id, std::vector<Id> &out) {
  const Id c = g.find(id);
  for (const PathLang &n : g[c].nodes) {
    if (isSeq(n)) {
      for (Id e : n.children()) {
        out.push_back(g.find(e));
      }
      return;
    }
  }
  out.push_back(c);
}

// If class `c` contains a JOIN e-node, copy its (canonicalized) member ids into
// `out` and return true (first join node wins).
inline bool joinMembers(Graph &g, Id c, std::vector<Id> &out) {
  for (const PathLang &n : g[c].nodes) {
    if (isJoin(n)) {
      out.clear();
      out.reserve(n.children().size());
      for (Id m : n.children()) {
        out.push_back(g.find(m));
      }
      return true;
    }
  }
  return false;
}

// Read-only mirror of canon::seq: the canonical e-class id of the sequence over
// `elems` IF it already exists, else nullopt. `elems` must be canonical
// (find'd) and flattened. Never mutates the e-graph.
inline std::optional<Id> lookupCanonSeq(const Graph &g,
                                        const std::vector<Id> &elems) {
  const auto zeroOpt = g.lookup(makeZero());
  const auto oneOpt = g.lookup(makeOne());
  std::vector<Id> kept;
  kept.reserve(elems.size());
  for (Id m : elems) {
    const Id mc = g.find(m);
    if (zeroOpt && mc == *zeroOpt) {
      return zeroOpt; // 0 · x = 0
    }
    if (oneOpt && mc == *oneOpt) {
      continue; // drop 1
    }
    kept.push_back(mc);
  }
  if (kept.empty()) {
    return oneOpt;
  }
  if (kept.size() == 1) {
    return kept.front();
  }
  return g.lookup(makeSeq(kept));
}

// A recorded expansion opportunity (a JOIN element inside a SEQ), already passed
// through the law gate and the alignment/growth guard.
struct ExpandAction {
  Id classId;              // the SEQ e-class gaining an alternative
  std::vector<Id> prefix;  // elems before the JOIN
  std::vector<Id> branches; // the JOIN's member classes
  std::vector<Id> suffix;  // elems after the JOIN
};

} // namespace detail

// One round of guarded expansion over the whole e-graph. Returns the number of
// e-classes that gained a genuinely new representation.
inline std::size_t expandRound(Graph &g, const LawProfile &L,
                               const ExtractOptions &opts) {
  const bool hasLeft = L.has(Law::LeftDistributive);
  const bool hasRight = L.has(Law::RightDistributive);
  if (!hasLeft && !hasRight) {
    return 0;
  }

  // Phase A: scan (read-only). Collect admitted expansion actions.
  std::vector<detail::ExpandAction> actions;
  for (Id c0 : g.classIds()) {
    const Id c = g.find(c0);
    for (const PathLang &node : g[c].nodes) {
      if (!isSeq(node)) {
        continue;
      }
      std::vector<Id> elems;
      elems.reserve(node.children().size());
      for (Id e : node.children()) {
        elems.push_back(g.find(e));
      }
      for (std::size_t i = 0; i < elems.size(); ++i) {
        std::vector<Id> branches;
        if (!detail::joinMembers(g, elems[i], branches)) {
          continue; // element i is not a JOIN
        }
        const bool hasPrefix = i > 0;
        const bool hasSuffix = i + 1 < elems.size();
        // Law gate: pulling a prefix into the join needs left distributivity;
        // pulling a suffix in needs right distributivity.
        if (hasPrefix && !hasLeft) {
          continue;
        }
        if (hasSuffix && !hasRight) {
          continue;
        }

        std::vector<Id> prefix(elems.begin(), elems.begin() + i);
        std::vector<Id> suffix(elems.begin() + i + 1, elems.end());

        // Opportunity guard: count branches whose expanded product already
        // exists (alignment), and the new e-nodes the expansion would add.
        std::size_t aligned = 0;
        std::size_t newNodes = 1; // the produced JOIN itself
        for (Id rm : branches) {
          std::vector<Id> flat = prefix;
          detail::appendClassElems(g, rm, flat);
          flat.insert(flat.end(), suffix.begin(), suffix.end());
          if (detail::lookupCanonSeq(g, flat)) {
            ++aligned;
          } else {
            ++newNodes;
          }
        }
        if (aligned < opts.expandMinAligned || newNodes > opts.expandGrowthCap) {
          continue;
        }

        detail::ExpandAction a;
        a.classId = c;
        a.prefix = std::move(prefix);
        a.branches = std::move(branches);
        a.suffix = std::move(suffix);
        actions.push_back(std::move(a));
      }
    }
  }

  // Phase B: materialize the expanded (distributed) join and unite it in.
  std::size_t changes = 0;
  for (auto &a : actions) {
    std::vector<Id> branchSeqs;
    branchSeqs.reserve(a.branches.size());
    for (Id rm : a.branches) {
      std::vector<Id> flat = a.prefix;
      detail::appendClassElems(g, rm, flat);
      flat.insert(flat.end(), a.suffix.begin(), a.suffix.end());
      branchSeqs.push_back(canon::seq(g, std::move(flat)));
    }
    const Id newJoin = canon::join(g, std::move(branchSeqs));
    if (g.uniteChecked(a.classId, newJoin).second) {
      ++changes;
    }
  }

  // canon::* dirties the e-graph even on memo hits; restore clean state.
  g.rebuild();
  return changes;
}

} // namespace ean
} // namespace elimination

