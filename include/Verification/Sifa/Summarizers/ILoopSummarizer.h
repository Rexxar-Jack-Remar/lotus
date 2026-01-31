//===-- Verification/Sifa/Summarizers/ILoopSummarizer.h -------------------===//
//
// Loop summarization interface (ported from Ultimate Library-Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ILOOPSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ILOOPSUMMARIZER_H

#include "Utils/General/PathExpressions/Regex.h"

namespace lotus {
namespace sifa {

template <typename L, typename StateT>
class ILoopSummarizer {
public:
  virtual ~ILoopSummarizer() = default;
  virtual StateT summarize(const lotus::pathexpressions::Star<L> &star, const StateT &input) = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ILOOPSUMMARIZER_H

