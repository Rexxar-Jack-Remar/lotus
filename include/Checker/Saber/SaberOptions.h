/** @file SaberOptions.h @brief Configuration options for SABER analysis. */
//===- SaberOptions.h -- Saber checker options ----------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// Uses LLVM cl::opt for engine-specific command-line options.
//
//===----------------------------------------------------------------------===//

#ifndef SABER_OPTIONS_H
#define SABER_OPTIONS_H

#include <llvm/Support/CommandLine.h>

namespace lotus {
namespace analysis {

// Forward declarations - defined in SaberOptions.cpp
extern llvm::cl::opt<bool> SaberFullSVFG;
extern llvm::cl::opt<unsigned> SaberCxtLimit;
extern llvm::cl::opt<unsigned> SaberMaxStepInWrapper;
extern llvm::cl::opt<unsigned> SaberMaxForwardItems;
extern llvm::cl::opt<unsigned> SaberZ3Timeout;
extern llvm::cl::opt<bool> SaberDumpSlice;
extern bool SaberValidateTests;
extern llvm::cl::opt<bool> SaberCollectExtRetGlobals;
extern bool SaberVerbose;

} // namespace analysis
} // namespace lotus

#endif
