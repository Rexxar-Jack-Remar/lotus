#pragma once

#include "Dataflow/NPA/Core/Domain.h"
#include "Dataflow/NPA/Domains/SparseFactSet.h"

namespace npa {

/**
 * Transfer-function carrier for finite Gen/Kill problems.
 *
 * A value represents f(x) = (x \ Kill) U Gen. Most kills and generations are
 * finite persistent sets. The additive identity needs Kill to be the entire
 * fact universe, so it is represented explicitly by kill_all.
 */
class GenKillTransformer {
public:
  using fact_type = SparseFactSet;

  struct value_type {
    bool kill_all = false;
    fact_type kill;
    fact_type gen;

    bool operator==(const value_type &other) const {
      return kill_all == other.kill_all && kill == other.kill &&
             gen == other.gen;
    }

    bool operator!=(const value_type &other) const { return !(*this == other); }
  };

  using test_type = bool;
  static constexpr bool idempotent = true;
  // This transfer-function carrier is only left-zero-annihilating:
  // composing a generating transformer after its zero() may generate facts.
  static constexpr bool sparse_npa_zero_left_annihilator = true;
  static constexpr bool sparse_npa_zero_right_annihilator = false;

  // Additive identity (no paths): f(x) = empty.
  static value_type zero() { return {true, {}, {}}; }

  // Multiplicative identity (no-op): f(x) = x.
  static value_type one() { return {}; }

  static value_type generate(unsigned bit) {
    value_type result;
    result.gen.set(bit);
    return result;
  }

  static value_type star(const value_type &value) {
    value_type result;
    result.gen = value.gen;
    return result;
  }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }

  static value_type combine(const value_type &a, const value_type &b) {
    value_type result;

    // K_new = K_a intersection K_b. A universal kill intersected with a
    // finite kill is that finite kill.
    if (a.kill_all && b.kill_all) {
      result.kill_all = true;
    } else if (a.kill_all) {
      result.kill = b.kill;
    } else if (b.kill_all) {
      result.kill = a.kill;
    } else {
      result.kill = a.kill;
      result.kill &= b.kill;
    }

    result.gen = a.gen;
    result.gen |= b.gen;
    return result;
  }

  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }

  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }

  // extend(a, b) means "apply a after b" (a composed with b).
  static value_type extend(const value_type &a, const value_type &b) {
    value_type result;

    // K = K_inner union K_outer.
    result.kill_all = a.kill_all || b.kill_all;
    if (!result.kill_all) {
      result.kill = b.kill;
      result.kill |= a.kill;
    }

    // G = (G_inner \ K_outer) union G_outer.
    if (!a.kill_all) {
      result.gen = b.gen;
      result.gen.intersectWithComplement(a.kill);
    }
    result.gen |= a.gen;
    return result;
  }

  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }

  static value_type subtract(const value_type &a, const value_type &) {
    // Gen/Kill transfer functions do not have a useful subtraction. Newton's
    // idempotent path uses nondeterministic combine instead.
    return a;
  }

  static fact_type apply(const value_type &summary, const fact_type &fact) {
    fact_type result;
    if (!summary.kill_all) {
      result = fact;
      result.intersectWithComplement(summary.kill);
    }
    result |= summary.gen;
    return result;
  }
};

} // namespace npa

