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
  mgr.generate_json_report(os);
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
  report->append_step(makeStep("sample.c", 9, "Null pointer dereference",
                               "callee", {NodeTag::RETURN_SITE}, "result"));

  ASSERT_TRUE(mgr.insert_report(bugType, report, false));

  std::string sarif;
  llvm::raw_string_ostream os(sarif);
  mgr.generate_sarif_report(os);
  os.flush();

  EXPECT_NE(sarif.find("Return from function callee. Access result. Null "
                       "pointer dereference"),
            std::string::npos);
  EXPECT_NE(sarif.find("Enter function callee. Access argument. Pointer may be "
                       "null"),
            std::string::npos);
}

} // namespace
