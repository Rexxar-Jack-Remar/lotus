//===-- Verification/Sifa/Interpreter/NoOpEnterCallRegistrar.h
//-------------===//
//
// No-op implementation of IEnterCallRegistrar (e.g. intraprocedural only).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Verification/Sifa/Interpreter/IEnterCallRegistrar.h"

#include <string>

namespace lotus {
namespace sifa {

template <typename StateT>
class NoOpEnterCallRegistrar final : public IEnterCallRegistrar<StateT> {
public:
  void registerEnterCall(const std::string &calleeName,
                         const StateT &calleeInput) override {
    (void)calleeName;
    (void)calleeInput;
  }
};

} // namespace sifa
} // namespace lotus

