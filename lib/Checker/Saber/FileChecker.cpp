//===- FileChecker.cpp -- File operation checker
//------------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/FileChecker.h"

#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugTypes.h"
#include "Checker/Saber/SaberCheckerAPI.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::analysis;

static void appendPathConditionEvents(BugReport *report, const ProgSlice *slice) {
  if (!report || !slice)
    return;
  ProgSlice::EventStack events;
  slice->evalFinalCond2Event(events);
  for (const auto &e : events) {
    if (!e.first)
      continue;
    const std::vector<NodeTag> tags = {
        e.second ? NodeTag::CONDITION_TRUE : NodeTag::CONDITION_FALSE};
    report->append_step(const_cast<Instruction *>(e.first), "Path condition", 0,
                        tags);
  }
}

void FileChecker::reportBug(ProgSlice *slice) {
  const SVFGNode *source = slice->getSource();
  if (!source)
    return;

  // Match SVF: only report when there is a file descriptor leak (never close
  // or partial close). Skip when all paths reach a close.
  const bool allReachable = isAllPathReachable();
  const bool someReachable = isSomePathReachable();
  if (allReachable && someReachable)
    return;

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const bool neverClose = (!allReachable && !someReachable);
  int bugTypeId = mgr.register_bug_type(
      neverClose ? "File Descriptor Leak" : "File Descriptor Leak 2",
      BugDescription::BI_HIGH, BugDescription::BC_PERFORMANCE,
      "CWE-773, CWE-775");

  BugReport *report = new BugReport(bugTypeId);

  if (const Instruction *inst = source->getInstruction()) {
    report->append_step(const_cast<Instruction *>(inst), "File opened here");
  }
  if (!neverClose)
    appendPathConditionEvents(report, slice);

  for (auto it = slice->sinksBegin(), et = slice->sinksEnd(); it != et; ++it) {
    const SVFGNode *snk = *it;
    if (const Instruction *inst = snk->getInstruction()) {
      report->append_step(const_cast<Instruction *>(inst), "File closed here", 1);
    }
  }

  mgr.insert_report(bugTypeId, report, false);

  if (!allReachable && !someReachable)
    outs() << "File never closed (full leak) at ";
  else
    outs() << "File may not be closed on some paths (partial) at ";
  if (const Instruction *inst = source->getInstruction()) {
    if (const Function *F = inst->getFunction())
      outs() << F->getName();
  }
  outs() << "\n";
}
