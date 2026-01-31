//===-- Verification/Sifa/Fluid/IFluid.h ----------------------------------===//
//
// Fluid abstraction policy interface (ported from Ultimate Sifa).
//
// Fluids decide when to apply abstraction to avoid blow-up.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_FLUID_IFLUID_H
#define LOTUS_VERIFICATION_SIFA_FLUID_IFLUID_H

namespace lotus {
namespace sifa {

template <typename StateT>
class IFluid {
public:
  virtual ~IFluid() = default;
  virtual bool shallBeAbstracted(const StateT &state) const = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_FLUID_IFLUID_H

