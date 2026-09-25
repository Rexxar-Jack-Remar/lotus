/**
 * \file Statistics.h
 * \brief Module statistics collection and reporting utility
 * \author Lotus Team
 *
 * This file provides utilities for collecting and reporting statistics
 * about LLVM modules, such as instruction counts and pointer usage.
 */
#pragma once

#include <llvm/IR/Module.h>

using namespace llvm;

/**
 * \class Statistics
 * \brief Module statistics collector
 *
 * This class provides methods to analyze LLVM modules and collect
 * various statistics such as instruction counts, pointer usage,
 * and memory access patterns.
 */
class Statistics {
public:
  static void run(Module &);
};

