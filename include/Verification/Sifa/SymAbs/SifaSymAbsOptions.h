//===-- Verification/Sifa/SymAbs/SifaSymAbsOptions.h ----------------------===//
//
// Configuration options for Sifa + SymbolicAbstraction-backed domain.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSOPTIONS_H
#define LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSOPTIONS_H

#include <string>

namespace lotus {
namespace sifa {

struct SifaSymAbsOptions {
  /// SymbolicAbstraction domain specification string.
  ///
  /// Examples (domain names come from SymbolicAbstraction registrations):
  /// - "Interval"
  /// - "Octagon"
  /// - "Interval, Octagon"
  std::string abstractDomain = "Interval, Octagon";

  /// SymbolicAbstraction analyzer variant to use.
  /// Supported values include "UnilateralAnalyzer", "BilateralAnalyzer", "OMTAnalyzer".
  std::string analyzerVariant = "UnilateralAnalyzer";

  /// Whether to recursively analyze callees when encoding call semantics.
  bool recursive = true;

  /// Validate that the analyzed LLVM IR is within a well-defined supported
  /// subset (suitable for C/C++ bitcode after "taming" passes).
  ///
  /// When enabled, unsupported constructs cause `analyzeSymAbs*()` to throw
  /// `std::invalid_argument` with a diagnostic listing the first issues found.
  bool validateLlvmSubset = true;

  /// Allow IEEE double-precision floating-point (`double`) values/ops.
  ///
  /// Note: `float` and FP trunc/ext are currently not supported.
  bool allowDouble = false;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSOPTIONS_H
