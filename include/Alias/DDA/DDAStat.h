//===- DDAStat.h -- DDA statistics (SVF-style) ---------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <set>
#include <string>

namespace lotus {
namespace analysis {

class DemandDrivenAA;

/// Statistics for demand-driven analysis (steps, strong updates, etc.).
class DDAStat {
public:
  explicit DDAStat(DemandDrivenAA *pta);

  uint32_t numOfDPM = 0;
  uint32_t numOfStrongUpdates = 0;
  uint32_t numOfMustAliases = 0;
  uint32_t numOfInfeasiblePath = 0;
  uint64_t numOfStep = 0;
  uint64_t numOfStepInCycle = 0;
  double anaTimePerQuery = 0.0;
  double totalTimeOfQueries = 0.0;
  double totalTimeOfBKCondition = 0.0;
  std::set<uint32_t> strongUpdateStores;

  void performStat();
  void printStat(const std::string &str = "");

private:
  DemandDrivenAA *pta_ = nullptr;
};

} // namespace analysis
} // namespace lotus
