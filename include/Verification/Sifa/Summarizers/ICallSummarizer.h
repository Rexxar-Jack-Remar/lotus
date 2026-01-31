//===-- Verification/Sifa/Summarizers/ICallSummarizer.h -------------------===//
//
// Call summarization interface (ported from Ultimate Library-Sifa).
//
// lotus v1 (intraprocedural) does not use this yet, but the interface is kept
// to preserve the structure of the original library.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ICALLSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ICALLSUMMARIZER_H

#include <string>

namespace lotus {
namespace sifa {

template <typename StateT>
class ICallSummarizer {
public:
  virtual ~ICallSummarizer() = default;
  virtual StateT summarize(const std::string &calleeName, const StateT &inputAfterCall) = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ICALLSUMMARIZER_H

