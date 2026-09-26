#pragma once

#include <functional>

#include <llvm/ADT/StringRef.h>

namespace llvm {
class raw_ostream;
} // namespace llvm

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

/// Write checker output through a temporary file and atomically replace the
/// destination after the stream has closed successfully.
bool writeCheckerOutputAtomically(
    llvm::StringRef path, llvm::StringRef format,
    const std::function<void(llvm::raw_ostream &)> &write);

int emitCheckerReports(BugReportMgr &manager,
                       const CheckerReportOptions &options = {});

} // namespace lotus::checker::tooling
