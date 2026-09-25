//===-- Verification/Sifa/Summarizers/ICallSummarizer.h -------------------===//
//
// Call summarization interface (ported from Ultimate Library-Sifa).
//
// Paper (TACAS 2020 "Ultimate Taipan..."): the call summarization operator
// computes a summary for a procedure call. IcfgInterpreter uses this when
// interpreting ReturnSummary transitions.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

namespace lotus {
namespace sifa {

template <typename StateT> class ICallSummarizer {
public:
  virtual ~ICallSummarizer() = default;
  virtual StateT summarize(const std::string &calleeName,
                           const StateT &inputAfterCall) = 0;
};

} // namespace sifa
} // namespace lotus

