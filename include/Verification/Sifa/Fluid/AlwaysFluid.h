//===-- Verification/Sifa/Fluid/AlwaysFluid.h -----------------------------===//
//
// Always abstract (ported from Ultimate Sifa).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Verification/Sifa/Fluid/IFluid.h"

namespace lotus {
namespace sifa {

template <typename StateT> class AlwaysFluid final : public IFluid<StateT> {
public:
  bool shallBeAbstracted(const StateT &state) const override {
    (void)state;
    return true;
  }
};

} // namespace sifa
} // namespace lotus

