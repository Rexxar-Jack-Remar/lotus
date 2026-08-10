#pragma once

class BugReportMgr;

namespace lotus::checker::tooling {

constexpr int EXIT_SUCCESS_CODE = 0;
constexpr int EXIT_FINDINGS = 1;
constexpr int EXIT_ERROR = 2;

struct CheckerReportOptions {
  bool verbose = false;
  int minScore = 0;
  bool printText = true;
};

/// Validate command-line options shared by every checker engine.
bool validateReportOptions();

int emitCheckerReports(BugReportMgr &manager,
                       const CheckerReportOptions &options = {});

} // namespace lotus::checker::tooling
