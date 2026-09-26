/**
 * \file Debug.h
 * \brief Debugging utilities for development and troubleshooting
 * \author Lotus Team
 *
 * This file provides debugging macros and utilities for conditional
 * logging and debugging output.
 */
#pragma once

#include <llvm/Support/raw_ostream.h>

#ifdef NDEBUG
#define POPEYE_WARN(X)
#else
#define POPEYE_WARN(X) llvm::outs() << "[WARN] " << X << "\n"
#endif

#define POPEYE_INFO(X) llvm::outs() << "[INFO] " << X << "\n"

/// @{
bool isPopeyeCurrentDebugType(const char *Type);

#define POPEYE_DEBUG_WITH_TYPE(TYPE, X)                                        \
  do {                                                                         \
    if (PopeyeDebugFlag && isPopeyeCurrentDebugType(TYPE)) {                   \
      X;                                                                       \
    }                                                                          \
  } while (false)

#define POPEYE_DEBUG(X) POPEYE_DEBUG_WITH_TYPE(DEBUG_TYPE, X)

extern bool PopeyeDebugFlag;
/// @}

