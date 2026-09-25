#pragma once

// TranslAPA baseline — Gen/Kill semiring (paper §4).
//
// This is the closed-form APA semantic function for separable (Gen/Kill)
// dataflow problems produced by the mechanical IDA->APA translation of
//   Zhou, Wang, Wang. "Mechanically Translating Iterative Dataflow Analysis to
//   Algebraic Program Analysis." OOPSLA 2026.
//
// A program property is a pair (Gen, Kill) of subsets of the finite fact
// universe D, represented as fixed-width llvm::BitVectors. The element denotes
// the transfer function
//     f(x) = Gen | (x & ~Kill).
//
// The composition rules (§4.3) are closed form; crucially star() is O(1) with
// NO fixpoint iteration, which is the whole point of the algebraic baseline and
// the key contrast with the framework's generic tree-walking interpreter
// (Solver/Interpretation/FactInterpreter.h, which iterates Star to a lattice fixpoint).
//
// Orientation contract: seq(L, R) means "L applied first, then R", matching
// PathExprFactory Concat semantics used by SolverContext::eval (Concat.L is
// evaluated on the incoming fact first, then Concat.R on the result). See the
// derivation in seq() below.

#include "llvm/ADT/BitVector.h"

#include <utility>

namespace elimination {
namespace translapa {

// (Gen, Kill) program property over a universe of |D| facts. Both vectors have
// width |D|; bit i corresponds to fact i in the translator's index space.
struct GenKillFact {
  llvm::BitVector Gen;
  llvm::BitVector Kill;

  bool operator==(const GenKillFact &Other) const {
    return Gen == Other.Gen && Kill == Other.Kill;
  }
  bool operator!=(const GenKillFact &Other) const { return !(*this == Other); }
};

// Closed-form Gen/Kill semiring. Constructed with the universe size |D| so that
// every produced element has consistent bit width.
class GenKillSemiring {
public:
  using Elem = GenKillFact;

  explicit GenKillSemiring(unsigned UniverseSize) : N(UniverseSize) {}

  unsigned universeSize() const { return N; }

  // Multiplicative unit (empty path ε): identity function f(x) = x.
  //   Gen = {}, Kill = {}.
  Elem one() const { return {emptyBV(), emptyBV()}; }

  // Additive unit (no path, PathExprFactory Zero): the union-neutral element.
  // f(x) must be absorbed by join, so Gen = {} and Kill = D (full), since
  // join's Kill is intersection (Kill & D = Kill) and Gen is union (Gen | {}).
  Elem zero() const { return {emptyBV(), fullBV()}; }

  // Atom: the (Gen, Kill) parameters of a single edge's transfer, as supplied
  // by the mechanical translator.
  Elem atom(llvm::BitVector Gen, llvm::BitVector Kill) const {
    return {std::move(Gen), std::move(Kill)};
  }

  // Sequence "L then R" = R . L. Derivation:
  //   R(L(x)) = R.Gen | (L(x) & ~R.Kill)
  //           = R.Gen | ((L.Gen | (x & ~L.Kill)) & ~R.Kill)
  //           = R.Gen | (L.Gen & ~R.Kill) | (x & ~(L.Kill | R.Kill))
  // hence Gen = R.Gen | (L.Gen & ~R.Kill), Kill = L.Kill | R.Kill
  // (paper's Gen_{12}=Gen_2 | (Gen_1\Kill_2), Kill_{12}=Kill_1|Kill_2).
  Elem seq(const Elem &L, const Elem &R) const {
    llvm::BitVector Gen = L.Gen;
    Gen.reset(R.Kill); // L.Gen \ R.Kill
    Gen |= R.Gen;
    llvm::BitVector Kill = L.Kill;
    Kill |= R.Kill;
    return {std::move(Gen), std::move(Kill)};
  }

  // Choice (branch join): Gen = Gen_1 | Gen_2, Kill = Kill_1 & Kill_2 (§4.3.2).
  Elem join(const Elem &L, const Elem &R) const {
    llvm::BitVector Gen = L.Gen;
    Gen |= R.Gen;
    llvm::BitVector Kill = L.Kill;
    Kill &= R.Kill;
    return {std::move(Gen), std::move(Kill)};
  }

  // Kleene star: Gen* = Gen, Kill* = {} (§4.3.3). Closed form, no iteration.
  Elem star(const Elem &A) const { return {A.Gen, emptyBV()}; }

  // Apply the summary to an incoming fact set: f(In) = Gen | (In & ~Kill).
  llvm::BitVector apply(const Elem &S, const llvm::BitVector &In) const {
    llvm::BitVector Out = In;
    Out.reset(S.Kill); // In \ Kill
    Out |= S.Gen;
    return Out;
  }

private:
  llvm::BitVector emptyBV() const { return llvm::BitVector(N, false); }
  llvm::BitVector fullBV() const { return llvm::BitVector(N, true); }

  unsigned N;
};

} // namespace translapa
} // namespace elimination

