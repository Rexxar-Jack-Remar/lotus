#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"

#include <string>
#include <vector>

#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

namespace {

BugDiagStep *makeStep(const std::string &file, int line, const std::string &tip,
                      const std::string &func = "",
                      const std::vector<NodeTag> &tags = {},
                      const std::string &access = "") {
  auto *step = new BugDiagStep();
  step->src_file = file;
  step->src_line = line;
  step->tip = tip;
  step->func_name = func;
  step->node_tags = tags;
  step->access = access;
  return step;
}

TEST(ReportFormattingTest, JsonExportIncludesNarrativeField) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Use After Free", BugDescription::BI_HIGH,
                                      BugDescription::BC_SECURITY, "CWE-416");

  auto *report = new BugReport(bugType);
  report->append_step(makeStep("main.c", 10, "Pointer escapes into callee",
                               "foo", {NodeTag::CALL_SITE}, "path"));
  report->append_step(
      makeStep("main.c", 18, "Dereference after free", "foo", {}, "memory"));

  ASSERT_TRUE(mgr.insert_report(bugType, report, false));

  std::string json;
  llvm::raw_string_ostream os(json);
  mgr.generate_json_report(os, {});
  os.flush();

  EXPECT_NE(json.find("\"Narrative\": \"Enter function foo. Access path. "
                      "Pointer escapes into callee\""),
            std::string::npos);
  EXPECT_NE(json.find("\"Narrative\": \"Access memory. Dereference after free\""),
            std::string::npos);
}

TEST(ReportFormattingTest, SarifUsesRenderedNarrativeMessages) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type(
      "Null Pointer Dereference", BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY, "CWE-476");

  auto *report = new BugReport(bugType);
  report->append_step(makeStep("sample.c", 4, "Pointer may be null", "callee",
                               {NodeTag::CALL_SITE}, "argument"));
  report->append_step(makeStep("sink.c", 9, "Null pointer dereference",
                               "callee", {NodeTag::RETURN_SITE}, "result"));

  ASSERT_TRUE(mgr.insert_report(bugType, report, false));

  std::string sarif;
  llvm::raw_string_ostream os(sarif);
  mgr.generate_sarif_report(os, {});
  os.flush();

  EXPECT_NE(sarif.find("Return from function callee. Access result. Null "
                       "pointer dereference"),
            std::string::npos);
  EXPECT_NE(sarif.find("Enter function callee. Access argument. Pointer may be "
                       "null"),
            std::string::npos);
  const size_t locations = sarif.find("\"locations\"");
  ASSERT_NE(locations, std::string::npos);
  EXPECT_LT(sarif.find("sink.c", locations),
            sarif.find("\"codeFlows\"", locations));
}

TEST(ReportFormattingTest, JsonTotalsUseTheSameFilterAsReports) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Filtered Bug");

  auto *included = new BugReport(bugType);
  included->set_conf_score(90);
  included->append_step(makeStep("filter.c", 10, "included"));
  ASSERT_TRUE(mgr.insert_report(bugType, included, false));

  auto *lowScore = new BugReport(bugType);
  lowScore->set_conf_score(20);
  lowScore->append_step(makeStep("filter.c", 20, "low score"));
  ASSERT_TRUE(mgr.insert_report(bugType, lowScore, false));

  auto *invalid = new BugReport(bugType);
  invalid->set_conf_score(100);
  invalid->set_valid(false);
  invalid->append_step(makeStep("filter.c", 30, "invalid"));
  ASSERT_TRUE(mgr.insert_report(bugType, invalid, false));

  std::string json;
  llvm::raw_string_ostream os(json);
  mgr.generate_json_report(os, BugReportMgr::ReportFilter{80, false});
  os.flush();

  EXPECT_NE(json.find("\"TotalBugs\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"TotalReports\": 1"), std::string::npos);
  EXPECT_EQ(json.find("low score"), std::string::npos);
  EXPECT_EQ(json.find("invalid"), std::string::npos);
}

TEST(ReportFormattingTest, ExactTraceDedupKeepsDistinctPathsToSameEndpoint) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Path Bug");

  auto *first = new BugReport(bugType);
  first->append_step(makeStep("path.c", 1, "first source"));
  first->append_step(makeStep("path.c", 20, "shared sink"));
  ASSERT_TRUE(mgr.insert_report(bugType, first, false));

  auto *second = new BugReport(bugType);
  second->append_step(makeStep("path.c", 2, "second source"));
  second->append_step(makeStep("path.c", 20, "shared sink"));
  ASSERT_TRUE(mgr.insert_report(bugType, second, false));

  mgr.deduplicate_reports(BugReportMgr::DedupMode::ExactTrace);
  EXPECT_EQ(mgr.get_total_reports(), 2);
}

TEST(ReportFormattingTest, EndpointDedupCanExplicitlyGroupSharedSinks) {
  BugReportMgr mgr;
  int bugType = mgr.register_bug_type("Grouped Bug");

  auto *first = new BugReport(bugType);
  first->append_step(makeStep("group.c", 1, "first source"));
  first->append_step(makeStep("group.c", 20, "shared sink"));
  ASSERT_TRUE(mgr.insert_report(bugType, first, false));

  auto *second = new BugReport(bugType);
  second->append_step(makeStep("group.c", 2, "second source"));
  second->append_step(makeStep("group.c", 20, "shared sink"));
  ASSERT_TRUE(mgr.insert_report(bugType, second, false));

  mgr.deduplicate_reports(BugReportMgr::DedupMode::Endpoint);
  EXPECT_EQ(mgr.get_total_reports(), 1);
}

TEST(ReportFormattingTest, DedupNeverMergesDifferentBugTypes) {
  BugReportMgr mgr;
  int firstType = mgr.register_bug_type("First Bug");
  int secondType = mgr.register_bug_type("Second Bug");

  auto *first = new BugReport(firstType);
  first->append_step(makeStep("types.c", 10, "same trace"));
  ASSERT_TRUE(mgr.insert_report(firstType, first, false));

  auto *second = new BugReport(secondType);
  second->append_step(makeStep("types.c", 10, "same trace"));
  ASSERT_TRUE(mgr.insert_report(secondType, second, false));

  mgr.deduplicate_reports(BugReportMgr::DedupMode::ExactTrace);
  EXPECT_EQ(mgr.get_total_reports(), 2);
}

} // namespace
