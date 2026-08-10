#pragma once

#include <string>

class BugReportMgr;

namespace lotus::checker::tooling {

constexpr int EXIT_SUCCESS_CODE = 0;
constexpr int EXIT_FINDINGS = 1;
constexpr int EXIT_ERROR = 2;

struct CheckerReportOptions {
  bool verbose = false;
  int minScore = 0;
  std::string jsonOutputOverride;
};

int emitCheckerReports(BugReportMgr &manager,
                       const CheckerReportOptions &options = {});

} // namespace lotus::checker::tooling
