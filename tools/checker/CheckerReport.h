#pragma once

class BugReportMgr;

namespace lotus::checker::tooling {

constexpr int EXIT_SUCCESS_CODE = 0;
constexpr int EXIT_FINDINGS = 1;
constexpr int EXIT_ERROR = 2;

struct CheckerReportOptions {
  bool verbose = false;
  int minScore = 0;
};

int emitCheckerReports(BugReportMgr &manager,
                       const CheckerReportOptions &options = {});

} // namespace lotus::checker::tooling
