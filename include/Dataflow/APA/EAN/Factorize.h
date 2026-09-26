#pragma once

// Factorize: variadic prefix/suffix factorization over JOIN e-classes
// (paper §III.B.b; Table I left/right distributivity). This is the FACTOR-phase
// primitive; the saturation loop that schedules it lives in M3.
//
// Binary distributivity rules cannot express factorization once choice and
// sequence are variadic (e.g. a JOIN holding a·b·c, a·b·d, x). So this is a
// direct e-graph pass rather than a string-pattern rewrite: for each JOIN
// e-class it groups the SEQ members by their first (prefix) / last (suffix)
// element e-class, extends each group to its longest common prefix/suffix P,
// and adds
//     JOIN( P·(r1 ⊕ … ⊕ rk),  <non-participating members…> )
// as an equivalent representation of the whole class (partial grouping keeps
// members that don't share the factor). The dual holds for suffixes:
//     JOIN( (r1 ⊕ … ⊕ rk)·P,  <non-participating members…> ).
//
// The original JOIN e-node is NOT removed — equality saturation retains both
// forms; extraction (M4) later chooses. Nesting (residual joins that factor
// again) is produced by repeated rounds.
//
// Correctness gating: prefix requires Law::LeftDistributive, suffix requires
// Law::RightDistributive.
//
// Two phases avoid mutating the e-graph while scanning it: Phase A only reads
// (collects factoring "ingredients" as plain Id lists); Phase B builds nodes
// (canon::*) and unites. factorizeRound rebuilds at the end and returns the
// number of classes that actually gained a new representation, so a caller can
// iterate to a fixpoint.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Dataflow/APA/EAN/Canonical.h"
#include "Dataflow/APA/EAN/LawProfile.h"
#include "Dataflow/APA/EAN/PathLang.h"

namespace elimination {
namespace ean {
namespace detail {

// If `member`'s e-class contains a SEQ e-node, copy its (canonicalized) element
// ids into `out` and return true. SEQ nodes always have >= 2 elements (canon
// collapses shorter ones), so a SEQ member has a well-defined first and last.
inline bool seqElems(Graph &g, Id member, std::vector<Id> &out) {
  const auto &cls = g[member];
  for (const PathLang &n : cls.nodes) {
    if (isSeq(n)) {
      out.clear();
      out.reserve(n.children().size());
      for (Id e : n.children()) {
        out.push_back(g.find(e));
      }
      return true;
    }
  }
  return false;
}

// A recorded factoring opportunity, resolved into concrete e-nodes in Phase B.
struct FactorAction {
  Id classId;                            // the JOIN e-class to add a form to
  bool prefix;                           // true: P·(⊕r);  false: (⊕r)·P
  std::vector<Id> fixedSide;             // the shared factor P (in order)
  std::vector<std::vector<Id>> residuals; // varying part per participating member
  std::vector<Id> passthrough;           // non-participating members, kept as-is
};

inline void scanPrefix(Id classId, const std::vector<Id> &members,
                       const std::vector<char> &isSeqMember,
                       const std::vector<std::vector<Id>> &elems,
                       std::vector<FactorAction> &out) {
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (isSeqMember[i]) {
      groups[elems[i].front().value()].push_back(i);
    }
  }
  for (auto &kv : groups) {
    const auto &idxs = kv.second;
    if (idxs.size() < 2) {
      continue;
    }
    std::size_t minLen = SIZE_MAX;
    for (std::size_t i : idxs) {
      minLen = std::min(minLen, elems[i].size());
    }
    std::size_t lcp = 0; // longest common prefix length (>= 1, shared first elem)
    for (; lcp < minLen; ++lcp) {
      Id ref = elems[idxs[0]][lcp];
      bool allEq = true;
      for (std::size_t i : idxs) {
        if (!(elems[i][lcp] == ref)) {
          allEq = false;
          break;
        }
      }
      if (!allEq) {
        break;
      }
    }
    FactorAction a;
    a.classId = classId;
    a.prefix = true;
    a.fixedSide.assign(elems[idxs[0]].begin(), elems[idxs[0]].begin() + lcp);
    std::vector<char> inGroup(members.size(), 0);
    for (std::size_t i : idxs) {
      inGroup[i] = 1;
      a.residuals.emplace_back(elems[i].begin() + lcp, elems[i].end());
    }
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (!inGroup[i]) {
        a.passthrough.push_back(members[i]);
      }
    }
    out.push_back(std::move(a));
  }
}

inline void scanSuffix(Id classId, const std::vector<Id> &members,
                       const std::vector<char> &isSeqMember,
                       const std::vector<std::vector<Id>> &elems,
                       std::vector<FactorAction> &out) {
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (isSeqMember[i]) {
      groups[elems[i].back().value()].push_back(i);
    }
  }
  for (auto &kv : groups) {
    const auto &idxs = kv.second;
    if (idxs.size() < 2) {
      continue;
    }
    std::size_t minLen = SIZE_MAX;
    for (std::size_t i : idxs) {
      minLen = std::min(minLen, elems[i].size());
    }
    std::size_t lcs = 0; // longest common suffix length (>= 1, shared last elem)
    for (; lcs < minLen; ++lcs) {
      Id ref = elems[idxs[0]][elems[idxs[0]].size() - 1 - lcs];
      bool allEq = true;
      for (std::size_t i : idxs) {
        if (!(elems[i][elems[i].size() - 1 - lcs] == ref)) {
          allEq = false;
          break;
        }
      }
      if (!allEq) {
        break;
      }
    }
    FactorAction a;
    a.classId = classId;
    a.prefix = false;
    const auto &sample = elems[idxs[0]];
    a.fixedSide.assign(sample.end() - lcs, sample.end());
    std::vector<char> inGroup(members.size(), 0);
    for (std::size_t i : idxs) {
      inGroup[i] = 1;
      a.residuals.emplace_back(elems[i].begin(), elems[i].end() - lcs);
    }
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (!inGroup[i]) {
        a.passthrough.push_back(members[i]);
      }
    }
    out.push_back(std::move(a));
  }
}

} // namespace detail

// Run one round of prefix/suffix factorization over the whole e-graph.
// Returns the number of e-classes that gained a genuinely new representation.
inline std::size_t factorizeRound(Graph &g, const LawProfile &L) {
  const bool doPrefix = L.has(Law::LeftDistributive);
  const bool doSuffix = L.has(Law::RightDistributive);
  if (!doPrefix && !doSuffix) {
    return 0;
  }

  // Phase A: scan (read-only w.r.t. e-graph structure — no add / no unite).
  std::vector<detail::FactorAction> actions;
  for (Id c : g.classIds()) {
    c = g.find(c);
    const auto &cls = g[c];
    for (const PathLang &node : cls.nodes) {
      if (!isJoin(node)) {
        continue;
      }
      std::vector<Id> members;
      members.reserve(node.children().size());
      for (Id m : node.children()) {
        members.push_back(g.find(m));
      }
      std::vector<char> isSeqM(members.size(), 0);
      std::vector<std::vector<Id>> elems(members.size());
      for (std::size_t i = 0; i < members.size(); ++i) {
        isSeqM[i] = detail::seqElems(g, members[i], elems[i]) ? 1 : 0;
      }
      if (doPrefix) {
        detail::scanPrefix(c, members, isSeqM, elems, actions);
      }
      if (doSuffix) {
        detail::scanSuffix(c, members, isSeqM, elems, actions);
      }
    }
  }

  // Phase B: materialize factored forms and unite them into their classes.
  std::size_t changes = 0;
  for (auto &a : actions) {
    std::vector<Id> resids;
    resids.reserve(a.residuals.size());
    for (auto &r : a.residuals) {
      resids.push_back(canon::seq(g, r)); // empty tail/head -> one
    }
    const Id residualJoin = canon::join(g, std::move(resids));

    std::vector<Id> factoredElems;
    if (a.prefix) {
      factoredElems = a.fixedSide;
      factoredElems.push_back(residualJoin);
    } else {
      factoredElems.push_back(residualJoin);
      factoredElems.insert(factoredElems.end(), a.fixedSide.begin(),
                           a.fixedSide.end());
    }
    const Id factoredSeq = canon::seq(g, std::move(factoredElems));

    std::vector<Id> newMembers;
    newMembers.reserve(1 + a.passthrough.size());
    newMembers.push_back(factoredSeq);
    for (Id p : a.passthrough) {
      newMembers.push_back(p);
    }
    const Id newJoin = canon::join(g, std::move(newMembers));

    if (g.uniteChecked(a.classId, newJoin).second) {
      ++changes;
    }
  }

  // canon::* uses g.add(), which marks the e-graph dirty even when nothing new
  // merges, so always restore a clean, queryable state before returning.
  g.rebuild();
  return changes;
}

// Convenience: iterate factorizeRound to a fixpoint (bounded). Returns total
// rounds that made progress. M3 will replace this with a budgeted scheduler.
inline std::size_t factorizeToFixpoint(Graph &g, const LawProfile &L,
                                       std::size_t maxRounds = 64) {
  std::size_t rounds = 0;
  for (; rounds < maxRounds; ++rounds) {
    if (factorizeRound(g, L) == 0) {
      break;
    }
  }
  return rounds;
}

} // namespace ean
} // namespace elimination

