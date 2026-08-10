#include "CheckerReport.h"

#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Fuzzing/TargetGeneration.h"

#include <algorithm>
#include <system_error>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus::checker::tooling {

int emitCheckerReports(BugReportMgr &manager,
                       const CheckerReportOptions &options) {
  if (!report_options::SuppressionFile.empty()) {
    SuppressionManager suppressions;
    if (!suppressions.loadFromFile(report_options::SuppressionFile)) {
      errs() << "Error loading suppressions from: "
             << report_options::SuppressionFile << "\n";
      return EXIT_ERROR;
    }
    manager.filterSuppressed(suppressions);
  }

  manager.deduplicate_reports(BugReportMgr::DedupMode::ExactTrace);
  const BugReportMgr::ReportFilter filter{
      std::max(options.minScore, report_options::MinConfidenceScore.getValue()),
      report_options::ShowInvalidReports.getValue()};

  manager.print_detailed_reports(outs(), options.verbose, filter);

  if (!report_options::TargetsOutputFile.empty()) {
    lotus::fuzzing::TargetGenerationOptions targetOptions;
    targetOptions.min_confidence_score = filter.minScore;
    targetOptions.include_invalid_reports = filter.includeInvalid;
    auto findings = lotus::fuzzing::collectFindings(manager, targetOptions);
    auto targets = lotus::fuzzing::collectTargets(findings);
    std::string errorMessage;
    if (!lotus::fuzzing::writeTargetsToFile(
            targets, report_options::TargetsOutputFile, &errorMessage)) {
      errs() << "Error writing fuzz targets: " << errorMessage << "\n";
      return EXIT_ERROR;
    }
    outs() << "\nFuzz targets written to: " << report_options::TargetsOutputFile
           << " (" << targets.size() << " targets)\n";
  }

  if (!report_options::JsonOutputFile.empty()) {
    std::error_code error;
    raw_fd_ostream output(report_options::JsonOutputFile, error,
                          sys::fs::OF_None);
    if (error) {
      errs() << "Error writing JSON report: " << error.message() << "\n";
      return EXIT_ERROR;
    }
    manager.generate_json_report(output, filter);
    outs() << "\nJSON report written to: " << report_options::JsonOutputFile
           << "\n";
  }

  if (!report_options::SarifOutputFile.empty()) {
    std::error_code error;
    raw_fd_ostream output(report_options::SarifOutputFile, error,
                          sys::fs::OF_None);
    if (error) {
      errs() << "Error writing SARIF report: " << error.message() << "\n";
      return EXIT_ERROR;
    }
    manager.generate_sarif_report(output, filter);
    outs() << "\nSARIF report written to: " << report_options::SarifOutputFile
           << "\n";
  }

  if (report_options::FailOnFindings &&
      manager.get_filtered_report_count(filter) > 0) {
    return EXIT_FINDINGS;
  }
  return EXIT_SUCCESS_CODE;
}

} // namespace lotus::checker::tooling
