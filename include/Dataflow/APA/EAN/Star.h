#pragma once

// Star-family rewrites (paper Table I, §III.C). M5 implements SLIDING:
//
//     (a·w)*·a  =  a·(w·a)*        (Kleene sliding; Law::Sliding)
//
// as a direct variadic-sequence pass, mirroring Factorize. It matches an
// adjacent pair inside a SEQ where the left element is a star whose body begins
// with the class that immediately follows it, and adds the "slid" form as an
// equivalent representation (the original is retained; extraction chooses).
// Star NORMALIZATION ((a*)*=a*, 0*=1*=1) is already enforced unconditionally by
// canon::star at construction, so it needs no rule here. Star UNFOLDING (which
// makes the e-graph cyclic) and the EXPLORE phase are deferred; cycle-safe
// extraction (M4) is already in place for when unfolding lands.
//
// Sliding does not create cycles; the rewritten star (w·a)* is an ordinary
// finite star. Termination follows from canonical hash-consing (re-running
// yields the same interned nodes → uniteChecked no-op).

#include <cstddef>
#include <utility>
#include <vector>

#include "Dataflow/APA/EAN/Canonical.h"
#include "Dataflow/APA/EAN/LawProfile.h"
#include "Dataflow/APA/EAN/PathLang.h"

namespace elimination {
namespace ean {
namespace detail {

// If class `c` contains a star e-node, set `bodyClass` to its body's canonical
// class id and return true (first star node wins — an M5 simplification).
inline bool starBodyClass(Graph &g, Id c, Id &bodyClass) {
  for (const PathLang &n : g[c].nodes) {
    if (isStar(n)) {
      bodyClass = g.find(n.children().front());
      return true;
    }
  }
  return false;
}

// Body element sequence: the SEQ elements of `bodyClass`, or `[bodyClass]` if
// the body is not a sequence (single atom/star/join).
inline void bodyElems(Graph &g, Id bodyClass, std::vector<Id> &out) {
  out.clear();
  for (const PathLang &n : g[bodyClass].nodes) {
    if (isSeq(n)) {
      out.reserve(n.children().size());
      for (Id e : n.children()) {
        out.push_back(g.find(e));
      }
      return;
    }
  }
  out.push_back(g.find(bodyClass));
}

struct SlideAction {
  Id classId;              // the SEQ e-class gaining an alternative
  std::vector<Id> prefix;  // elems[0 .. i-1]
  Id a{};                  // first(body) == elems[i+1]
  std::vector<Id> rotated; // body[1:] ++ [a]  -> new star body
  std::vector<Id> suffix;  // elems[i+2 .. ]
};

} // namespace detail

// One round of sliding over the whole e-graph. Returns the number of e-classes
// that gained a new representation.
inline std::size_t slideRound(Graph &g, const LawProfile &L) {
  if (!L.has(Law::Sliding)) {
    return 0;
  }

  // Phase A: scan (read-only — no add / no unite).
  std::vector<detail::SlideAction> actions;
  for (Id c0 : g.classIds()) {
    const Id c = g.find(c0);
    const auto &cls = g[c];
    for (const PathLang &node : cls.nodes) {
      if (!isSeq(node)) {
        continue;
      }
      std::vector<Id> elems;
      elems.reserve(node.children().size());
      for (Id e : node.children()) {
        elems.push_back(g.find(e));
      }
      for (std::size_t i = 0; i + 1 < elems.size(); ++i) {
        Id bodyClass;
        if (!detail::starBodyClass(g, elems[i], bodyClass)) {
          continue;
        }
        std::vector<Id> body;
        detail::bodyElems(g, bodyClass, body);
        if (body.empty() || !(body.front() == elems[i + 1])) {
          continue; // trailing element must equal the star body's first element
        }
        detail::SlideAction act;
        act.classId = c;
        act.prefix.assign(elems.begin(), elems.begin() + i);
        act.a = body.front();
        act.rotated.assign(body.begin() + 1, body.end());
        act.rotated.push_back(body.front());
        act.suffix.assign(elems.begin() + i + 2, elems.end());
        actions.push_back(std::move(act));
      }
    }
  }

  // Phase B: build the slid forms and unite them in.
  std::size_t changes = 0;
  for (auto &act : actions) {
    const Id newStar = canon::star(g, canon::seq(g, act.rotated));
    std::vector<Id> elems = act.prefix;
    elems.push_back(act.a);
    elems.push_back(newStar);
    elems.insert(elems.end(), act.suffix.begin(), act.suffix.end());
    const Id newSeq = canon::seq(g, std::move(elems));
    if (g.uniteChecked(act.classId, newSeq).second) {
      ++changes;
    }
  }

  // canon::* dirties the e-graph even on memo hits; always restore clean state.
  g.rebuild();
  return changes;
}

} // namespace ean
} // namespace elimination

