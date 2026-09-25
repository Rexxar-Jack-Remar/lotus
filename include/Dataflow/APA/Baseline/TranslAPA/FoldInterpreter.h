#pragma once

// TranslAPA baseline — memoized bottom-up fold over the path-expression DAG.
//
// APA's semantic function D is defined for an arbitrary path expression by the
// algebraic composition rules of the semiring. This interpreter evaluates the
// hash-consed PathExprFactory<TransferT> DAG produced by the framework's
// elimination front-end (Results.ExprTo(N)) into one semiring element per
// UNIQUE node, memoized on the Expr pointer.
//
// Cost is O(#unique DAG nodes), versus SolverContext::eval which recurses over
// the expanded tree and iterates Star to a fixpoint. Same path expression p,
// different semantic function — the apples-to-apples comparison this baseline
// exists for.

#include "Dataflow/APA/Core/PathExpr.h"

#include <cstddef>
#include <unordered_map>
#include <utility>

namespace elimination {
namespace translapa {

// SemiringT must provide: type Elem; and zero()/one()/star(Elem)/
// seq(Elem,Elem)/join(Elem,Elem). AtomFn maps a const TransferT& to Elem.
template <typename TransferT, typename SemiringT, typename AtomFn>
class FoldInterpreter {
public:
  using Factory = PathExprFactory<TransferT>;
  using Ref = typename Factory::Ref;
  using Expr = typename Factory::Expr;
  using Kind = typename Factory::Kind;
  using Elem = typename SemiringT::Elem;

  FoldInterpreter(const SemiringT &S, AtomFn Atom) : S(S), Atom(std::move(Atom)) {}

  Elem fold(const Ref &E) { return foldPtr(E.get()); }

  std::size_t uniqueNodesVisited() const { return Memo.size(); }

private:
  Elem foldPtr(const Expr *E) {
    if (E == nullptr) {
      return S.zero();
    }
    auto It = Memo.find(E);
    if (It != Memo.end()) {
      return It->second;
    }
    Elem V = compute(E);
    Memo.emplace(E, V);
    return V;
  }

  Elem compute(const Expr *E) {
    switch (E->K) {
    case Kind::Zero:
      return S.zero();
    case Kind::One:
      return S.one();
    case Kind::Atom:
      return Atom(*E->Transfer);
    case Kind::Union:
      return S.join(foldPtr(E->L.get()), foldPtr(E->R.get()));
    case Kind::Concat:
      // Concat.L is applied first (SolverContext::eval convention), so seq's
      // "L then R" orientation matches.
      return S.seq(foldPtr(E->L.get()), foldPtr(E->R.get()));
    case Kind::Star:
      return S.star(foldPtr(E->L.get()));
    }
    return S.zero();
  }

  const SemiringT &S;
  AtomFn Atom;
  std::unordered_map<const Expr *, Elem> Memo;
};

// Deduction helper so callers can write makeFoldInterpreter(S, atomFn) without
// spelling out the AtomFn type.
template <typename TransferT, typename SemiringT, typename AtomFn>
FoldInterpreter<TransferT, SemiringT, AtomFn>
makeFoldInterpreter(const SemiringT &S, AtomFn Atom) {
  return FoldInterpreter<TransferT, SemiringT, AtomFn>(S, std::move(Atom));
}

} // namespace translapa
} // namespace elimination

