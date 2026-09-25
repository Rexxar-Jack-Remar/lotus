#pragma once

// Import: PathExprFactory<TransferT>::Ref DAG  ->  canonical e-graph.
//
// This realizes invariants I1–I3 of the paper's import step (docs, §III.A):
//   I2 (profile-relative canonical form): variadic joins are flattened, have 0
//       removed, are deduplicated and sorted; variadic sequences are flattened
//       and have 1 removed but are NEVER reordered.
//   I3 (root preservation): every input root maps to an e-class id, returned in
//       order, so the original expression is always recoverable.
//
// Flattening is done on the *Ref tree* (PathExprFactory's Union/Concat are
// binary, so `(a⊕b)⊕c` is literally `Union(Union(a,b),c)`), which is cleaner
// than post-hoc flattening inside the e-graph and needs no e-graph state. The
// per-node canonical form is then produced by the shared canon:: builders.

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/EAN/AtomTable.h"
#include "Dataflow/APA/EAN/Canonical.h" // Graph + canon::join/seq/star
#include "Dataflow/APA/EAN/PathLang.h"

namespace elimination {
namespace ean {

template <typename TransferT> struct ImportResult {
  Graph g;
  std::vector<Id> roots;         // one canonical e-class id per input root (I3)
  AtomTable<TransferT> atoms;    // atom id <-> original Ref, for Export
};

namespace detail {

template <typename TransferT> class Importer {
public:
  using Factory = PathExprFactory<TransferT>;
  using Ref = typename Factory::Ref;
  using Kind = typename Factory::Kind;

  Graph g;
  std::vector<Id> roots;
  AtomTable<TransferT> atoms;

  Id importExpr(const Ref &e) {
    auto it = memo_.find(e.get());
    if (it != memo_.end()) {
      return g.find(it->second);
    }
    Id result = importFresh(e);
    memo_.emplace(e.get(), result);
    return result;
  }

private:
  Id importFresh(const Ref &e) {
    switch (e->K) {
    case Kind::Zero:
      return canon::zeroId(g);
    case Kind::One:
      return canon::oneId(g);
    case Kind::Atom:
      return g.add(makeAtom(atoms.intern(e)));
    case Kind::Union: {
      std::vector<Id> members;
      collectJoin(e, members);
      return canon::join(g, std::move(members));
    }
    case Kind::Concat: {
      std::vector<Id> members;
      collectSeq(e, members);
      return canon::seq(g, std::move(members));
    }
    case Kind::Star:
      return canon::star(g, importExpr(e->L));
    }
    return canon::zeroId(g); // unreachable; silences -Wreturn-type
  }

  // Flatten nested Union at the Ref level; leaves (non-Union) get imported.
  void collectJoin(const Ref &e, std::vector<Id> &out) {
    if (e->K == Kind::Union) {
      collectJoin(e->L, out);
      collectJoin(e->R, out);
    } else {
      out.push_back(importExpr(e));
    }
  }

  void collectSeq(const Ref &e, std::vector<Id> &out) {
    if (e->K == Kind::Concat) {
      collectSeq(e->L, out);
      collectSeq(e->R, out);
    } else {
      out.push_back(importExpr(e));
    }
  }

  std::unordered_map<const typename Factory::Expr *, Id> memo_;
};

} // namespace detail

// Import a batch of path-expression roots into a canonical e-graph.
template <typename TransferT>
ImportResult<TransferT>
importCanonical(const std::vector<typename PathExprFactory<TransferT>::Ref> &R) {
  detail::Importer<TransferT> imp;
  imp.roots.reserve(R.size());
  for (const auto &r : R) {
    imp.roots.push_back(imp.importExpr(r));
  }
  imp.g.rebuild();
  for (auto &id : imp.roots) {
    id = imp.g.find(id);
  }
  return ImportResult<TransferT>{std::move(imp.g), std::move(imp.roots),
                                 std::move(imp.atoms)};
}

} // namespace ean
} // namespace elimination

