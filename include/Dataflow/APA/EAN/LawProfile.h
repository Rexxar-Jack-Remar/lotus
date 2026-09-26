#pragma once

// LawProfile: the client-declared set of algebraic laws EAN is allowed to use
// when rewriting path expressions (paper §III.B.c, Table I; requirement R1).
//
// Only *exploratory* rewrites are gated here. The canonical representation
// laws (choice ACI, identities, annihilation) are enforced unconditionally by
// Import/Canonical, matching what the baseline PathExprFactory already does at
// construction time.
//
// A rewrite is enabled only when its required capability is present, so an
// optimizer for one client never imposes stronger algebraic assumptions on
// another. Correctness is relative to the client actually satisfying its
// declared profile (an incorrect declaration is a client bug, outside EAN's
// proof boundary).

#include <cstdint>

namespace elimination {
namespace ean {

enum class Law : unsigned {
  LeftDistributive = 0, // (a·b) ⊕ (a·c) = a·(b ⊕ c)     — prefix factorization
  RightDistributive,    // (b·a) ⊕ (c·a) = (b ⊕ c)·a     — suffix factorization
  Annihilation,         // a·0 = 0·a = 0
  KleeneStar,           // (a*)* = a*, 0* = 1* = 1
  LeftUnfold,           // a* = 1 ⊕ a·a*
  RightUnfold,          // a* = 1 ⊕ a*·a
  Sliding,              // (a·b)*·a = a·(b·a)*
  Count
};

class LawProfile {
public:
  LawProfile() = default;

  bool has(Law l) const { return (bits_ >> idx(l)) & 1u; }
  LawProfile &enable(Law l) {
    bits_ |= (1u << idx(l));
    return *this;
  }
  LawProfile &disable(Law l) {
    bits_ &= ~(1u << idx(l));
    return *this;
  }

  // A full Kleene-algebra client: every law enabled.
  static LawProfile kleeneAlgebra() {
    LawProfile p;
    p.enable(Law::LeftDistributive)
        .enable(Law::RightDistributive)
        .enable(Law::Annihilation)
        .enable(Law::KleeneStar)
        .enable(Law::LeftUnfold)
        .enable(Law::RightUnfold)
        .enable(Law::Sliding);
    return p;
  }

  // A weaker flow algebra: distributivity + annihilation, but no star laws.
  static LawProfile flowAlgebra() {
    LawProfile p;
    p.enable(Law::LeftDistributive)
        .enable(Law::RightDistributive)
        .enable(Law::Annihilation);
    return p;
  }

  static LawProfile none() { return LawProfile(); }

  // The universally-safe minimal profile: LEFT distributivity only. Left
  // factorization (a·b)⊕(a·c) = a·(b⊕c) holds unconditionally for the
  // compositional MOP interpretation (the shared prefix is applied once, then
  // the branches meet), so enabling EAN with this profile can never change any
  // client's result — including non-distributive clients like constant
  // propagation. Distributive clients may additionally enable RightDistributive
  // and the star laws.
  static LawProfile safeMinimal() {
    LawProfile p;
    p.enable(Law::LeftDistributive);
    return p;
  }

private:
  static unsigned idx(Law l) { return static_cast<unsigned>(l); }
  std::uint32_t bits_ = 0;
};

} // namespace ean
} // namespace elimination

