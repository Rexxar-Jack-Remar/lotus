/**
 * @file SMTSampler.h
 * @brief Main header for SMT sampling functionality
 *
 * This file provides the common includes and forward declarations for the SMT
 * sampling module.  The module implements various techniques for sampling
 * satisfying assignments (models) from SMT formulas, including:
 *
 * 1. QuickSampler: A mutation-based approach for generating diverse models.
 * 2. RegionSampler (PolySampler): A geometry-based approach for sampling from
 *    convex polytopes defined by linear constraints.
 * 3. IntervalSampler: An interval-based sampling strategy.
 *
 * These samplers are used for test case generation, solution space exploration,
 * and analyzing formula sensitivity.
 *
 * NOTE (L8 fix): "using namespace std" and "using namespace z3" have been
 * removed from this header.  They were polluting the namespace of every
 * translation unit that included this file.  Each .cpp file that needs these
 * namespaces should declare them locally.
 */

#pragma once

#include "Solvers/SMT/LIBSMT/Z3Plus.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <z3++.h>

// do NOT place "using namespace std" or "using namespace z3" here.
// Each .cpp implementation file declares them locally as needed.

namespace lotus::SMTSampler {

/// Run the DIMACS/CNF mutation sampler.
void runQuickSampler(const std::string &input_file, int max_samples = 1000,
                     double max_time_seconds = 30.0);

/// Run the interval sampler on an SMT-LIB input file or directory.
void runIntervalSampler(const std::string &input_path, int max_samples = 1000,
                        double max_time_ms = 30000.0);

/// Run the symbolic-abstraction region sampler on an SMT-LIB input file.
void runRegionSampler(const std::string &input_file, int max_samples = 1000,
                      double max_time_ms = 30000.0);

} // namespace lotus::SMTSampler
