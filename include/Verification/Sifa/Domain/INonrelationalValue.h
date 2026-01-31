//===-- Verification/Sifa/Domain/INonrelationalValue.h --------------------===//
//
// Interface for values in a NonrelationalState (Ultimate-aligned).
//
// Ultimate's NonrelationalState<VALUE> requires VALUE to extend
// INonrelationalValue: join(other), widen(other), isTop(), isBottom().
// Used for Interval, ExplicitValue (constant), etc.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_INONRELATIONALVALUE_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_INONRELATIONALVALUE_H

#include <concepts>

namespace lotus {
namespace sifa {

/// Concept for non-relational value (Ultimate INonrelationalValue).
/// Requirements: join(other), widen(other), isTop(), isBottom().
template <typename V>
concept INonrelationalValue = requires(const V &a, const V &b) {
  { a.join(b) } -> std::same_as<V>;
  { a.widen(b) } -> std::same_as<V>;
  { a.isTop() } -> std::same_as<bool>;
  { a.isBottom() } -> std::same_as<bool>;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_INONRELATIONALVALUE_H
