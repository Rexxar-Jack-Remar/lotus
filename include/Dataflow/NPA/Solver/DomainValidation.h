#pragma once

#include "Dataflow/NPA/Core/Domain.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace npa {

class DomainContractViolationError : public std::logic_error {
public:
  DomainContractViolationError()
      : std::logic_error("NPA domain failed runtime contract checks") {}
};

template <class D>
inline bool run_sampled_domain_contract_checks(
    const std::vector<DomVal<D>> &representative_values, bool verbose = false) {
  bool ok = true;
  const auto Zero = D::zero();
  const auto One = D::one();
  std::vector<DomVal<D>> values;
  values.reserve(representative_values.size() + 2U);
  values.push_back(Zero);
  values.push_back(One);
  values.insert(values.end(), representative_values.begin(),
                representative_values.end());
  auto Require = [&](bool Condition, const char *Message) {
    if (Condition)
      return;
    ok = false;
    if (verbose)
      std::cerr << "[npa-contract] " << Message << '\n';
  };

  Require(D::equal(Zero, Zero), "zero() must equal itself");
  Require(D::equal(One, One), "one() must equal itself");
  for (const auto &value : values) {
    Require(D::equal(D::combine(Zero, value), value),
            "zero must be a left identity for combine");
    Require(D::equal(D::combine(value, Zero), value),
            "zero must be a right identity for combine");
    Require(D::equal(D::extend(One, value), value),
            "one must be a left identity for extend");
    Require(D::equal(D::extend(value, One), value),
            "one must be a right identity for extend");
    Require(D::equal(D::extend(Zero, value), Zero),
            "zero must left-annihilate extend");
    Require(D::equal(D::extend(value, Zero), Zero),
            "zero must right-annihilate extend");
    Require(D::equal(D::extend_lin(One, value), D::extend(One, value)),
            "extend_lin must agree with extend");
    Require(D::equal(D::ndetCombine(Zero, value), D::combine(Zero, value)),
            "ndetCombine must agree with combine");
    if (D::idempotent)
      Require(D::equal(D::combine(value, value), value),
              "idempotent domain: value⊕value != value");
  }

  for (const auto &a : values) {
    for (const auto &b : values) {
      Require(D::equal(D::combine(a, b), D::combine(b, a)),
              "combine must be commutative");
      Require(D::equal(D::extend_lin(a, b), D::extend(a, b)),
              "extend_lin must agree with extend");
      Require(D::equal(D::ndetCombine(a, b), D::combine(a, b)),
              "ndetCombine must agree with combine");
      if constexpr (D::idempotent) {
        const bool a_leq_b = D::equal(D::combine(a, b), b);
        if (a_leq_b) {
          for (const auto &c : values) {
            Require(D::equal(D::combine(D::extend(a, c), D::extend(b, c)),
                             D::extend(b, c)),
                    "extend must be monotone in its left operand");
            Require(D::equal(D::combine(D::extend(c, a), D::extend(c, b)),
                             D::extend(c, b)),
                    "extend must be monotone in its right operand");
          }
        }
      }
      if constexpr (DomainHasProject<D>::value || DomainHasProjectT<D>::value) {
        const auto projected_a = domain_project<D>(a);
        const auto projected_b = domain_project<D>(b);
        Require(D::equal(domain_project<D>(D::combine(a, b)),
                         D::combine(projected_a, projected_b)),
                "project must distribute over combine");
        Require(D::equal(domain_project<D>(projected_a), projected_a),
                "project must be idempotent");
      }
      if constexpr (!D::idempotent) {
        const auto total = D::combine(a, b);
        DomVal<D> delta = [&]() {
          if constexpr (DomainHasChooseDelta<D>::value)
            return D::choose_delta(total, a);
          else
            return D::subtract(total, a);
        }();
        Require(D::equal(D::combine(a, delta), total),
                "delta must reconstruct the combined value");
      }
      for (const auto &c : values) {
        Require(D::equal(D::combine(D::combine(a, b), c),
                         D::combine(a, D::combine(b, c))),
                "combine must be associative");
        Require(D::equal(D::extend(D::extend(a, b), c),
                         D::extend(a, D::extend(b, c))),
                "extend must be associative");
        Require(D::equal(D::extend(a, D::combine(b, c)),
                         D::combine(D::extend(a, b), D::extend(a, c))),
                "extend must distribute over combine on the left");
        Require(D::equal(D::extend(D::combine(a, b), c),
                         D::combine(D::extend(a, c), D::extend(b, c))),
                "extend must distribute over combine on the right");
      }
    }
  }
  return ok;
}

template <class D>
inline bool run_basic_domain_contract_checks(bool verbose = false) {
  return run_sampled_domain_contract_checks<D>({}, verbose);
}

inline void require_domain_contract(bool contract_ok) {
  if (!contract_ok)
    throw DomainContractViolationError{};
}

} // namespace npa

