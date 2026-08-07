#ifndef NPA_SOLVER_DOMAIN_VALIDATION_H
#define NPA_SOLVER_DOMAIN_VALIDATION_H

#include "Dataflow/NPA/Core/Domain.h"

#include <iostream>

namespace npa {

template <class D>
inline bool run_basic_domain_contract_checks(bool verbose = false) {
  bool ok = true;
  if (!D::equal(D::zero(), D::zero())) {
    ok = false;
    if (verbose)
      std::cerr << "[npa-contract] zero() must equal itself\n";
  }
  if (!D::equal(D::one(), D::one())) {
    ok = false;
    if (verbose)
      std::cerr << "[npa-contract] one() must equal itself\n";
  }
  if (D::idempotent) {
    if (!D::equal(D::combine(D::zero(), D::zero()), D::zero())) {
      ok = false;
      if (verbose)
        std::cerr << "[npa-contract] idempotent domain: zero⊕zero != zero\n";
    }
    if (!D::equal(D::combine(D::one(), D::one()), D::one())) {
      ok = false;
      if (verbose)
        std::cerr << "[npa-contract] idempotent domain: one⊕one != one\n";
    }
  }
  return ok;
}

} // namespace npa

#endif // NPA_SOLVER_DOMAIN_VALIDATION_H
