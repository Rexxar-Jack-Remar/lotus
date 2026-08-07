#ifndef NPA_NEWTON_ERRORS_H
#define NPA_NEWTON_ERRORS_H

#include "Dataflow/NPA/Core/Domain.h"

#include <stdexcept>

namespace npa {

class InvalidNewtonDeltaError : public std::logic_error {
public:
  InvalidNewtonDeltaError()
      : std::logic_error("invalid Newton delta: non-idempotent domains must "
                         "provide subtract()/choose_delta() such that "
                         "combine(nu, delta) == f(nu)") {}
};

class UnsupportedNewtonMuError : public std::logic_error {
public:
  UnsupportedNewtonMuError()
      : std::logic_error("unsupported Newton expression: Mu is evaluable but "
                         "outside the paper-faithful Newton/tensor fragment") {}
};

class UnsafeNewtonProjectError : public std::logic_error {
public:
  UnsafeNewtonProjectError()
      : std::logic_error(
            "unsafe Newton projection: domains must opt in with "
            "project_newton_safe for Project on Newton/tensor paths") {}
};

template <class D>
inline bool valid_newton_delta(const DomVal<D> &f_nu, const DomVal<D> &nu,
                               const DomVal<D> &delta) {
  return domain_exact_equal<D>(D::combine(nu, delta), f_nu);
}

template <class D>
inline void require_valid_newton_delta(const DomVal<D> &f_nu,
                                       const DomVal<D> &nu,
                                       const DomVal<D> &delta) {
  if (!valid_newton_delta<D>(f_nu, nu, delta))
    throw InvalidNewtonDeltaError{};
}

} // namespace npa

#endif // NPA_NEWTON_ERRORS_H
