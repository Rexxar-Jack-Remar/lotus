/*
 * FiTx Bug Finder Tool (Standalone)
 *
 * FiTx: Framework for Finger Traceable Bugs in Linux
 * A static analysis framework for detecting common memory and concurrency bugs.
 *
 * Supported bug types:
 * - Double Free (df)
 * - Double Lock (dl)
 * - Double Unlock (dul)
 * - Memory Leak (leak)
 * - Null Pointer Dereference (nullptr)
 * - Use After Free (uaf)
 * - Use Before Initialization (ubi)
 * - Reference Count Issues (ref_count, ref_uncount)
 *
 * Usage: lotus-fitx [options] <input bitcode>
 */

#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Framework_IR/IRGenerator.h"
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string>
    InputFile(cl::Positional, cl::desc("<input bitcode>"), cl::Required,
              cl::sub(lotus::checker::tooling::fitxSubCommand()));
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false),
                             cl::sub(lotus::checker::tooling::fitxSubCommand()));
static cl::opt<bool> MeasureAnalysisTime("fitx-measure",
                                         cl::desc("Measure analysis time"),
                                         cl::init(false),
                                         cl::sub(lotus::checker::tooling::fitxSubCommand()));
static cl::opt<std::string>
    DetectorType("detector",
                 cl::desc("Detector type: all, df, dl, dul, leak, nullptr, "
                          "uaf, ubi, ref_count, ref_uncount"),
                 cl::init("all"),
                 cl::sub(lotus::checker::tooling::fitxSubCommand()));

namespace {
std::string formatLocation(const BugDiagStep *step) {
  if (!step || step->src_file.empty()) {
    return "unknown location";
  }

  std::string location = step->src_file + ":" + std::to_string(step->src_line);
  if (step->src_column > 0) {
    location += ":" + std::to_string(step->src_column);
  }
  return location;
}

const BugDiagStep *getPrimaryStep(const BugReport *report) {
  if (!report) {
    return nullptr;
  }

  const auto &steps = report->get_steps();
  for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
    if (*it && !(*it)->src_file.empty()) {
      return *it;
    }
  }
  return steps.empty() ? nullptr : steps.back();
}

void printDetailedReports(raw_ostream &OS, const BugReportMgr &mgr,
                          bool verbose) {
  int total = 0;
  for (size_t type_id = 0; type_id < mgr.get_num_bug_types(); ++type_id) {
    const auto *reports = mgr.get_reports_for_type(type_id);
    if (!reports) {
      continue;
    }
    for (const BugReport *report : *reports) {
      if (report->get_conf_score() < report_options::MinConfidenceScore) {
        continue;
      }
      if (!report_options::ShowInvalidReports && !report->is_valid()) {
        continue;
      }
      ++total;
    }
  }

  if (total == 0) {
    OS << "\nNo FiTx findings.\n";
    return;
  }

  OS << "\nFindings (" << total << ")\n";
  OS << "================\n";

  int index = 1;
  for (size_t type_id = 0; type_id < mgr.get_num_bug_types(); ++type_id) {
    const auto *reports = mgr.get_reports_for_type(type_id);
    if (!reports || reports->empty()) {
      continue;
    }

    const BugReportMgr::BugType &bug_type = mgr.get_bug_type_info(type_id);
    for (const BugReport *report : *reports) {
      if (report->get_conf_score() < report_options::MinConfidenceScore) {
        continue;
      }
      if (!report_options::ShowInvalidReports && !report->is_valid()) {
        continue;
      }

      const BugDiagStep *primary = getPrimaryStep(report);

      OS << "\n" << index++ << ". " << bug_type.bug_name;
      if (primary) {
        OS << "\n   Location: " << formatLocation(primary);
        if (!primary->func_name.empty()) {
          OS << " in " << primary->func_name;
        }
      }

      std::string message = report->render_primary_message();
      if (!message.empty()) {
        OS << "\n   Message: " << message;
      }

      if (primary && !primary->source_code.empty()) {
        OS << "\n   Source: " << primary->source_code;
      }

      if (verbose && primary && !primary->llvm_ir.empty()) {
        OS << "\n   LLVM IR: " << primary->llvm_ir;
      }

      const BugReportExtras *extras = report->get_extras();
      if (extras && !extras->suggestion.empty()) {
        OS << "\n   Suggestion: " << extras->suggestion;
      }

      if (verbose && extras) {
        auto it = extras->metadata.find("fitx_value");
        if (it != extras->metadata.end()) {
          OS << "\n   Analysis Value: " << it->second;
        }
      }

      if (verbose) {
        const auto &steps = report->get_steps();
        if (steps.size() > 1) {
          OS << "\n   Trace:";
          for (const BugDiagStep *step : steps) {
            if (!step) {
              continue;
            }
            OS << "\n     - ";
            if (!step->src_file.empty()) {
              OS << formatLocation(step) << ": ";
            }
            std::string step_message = report->render_step_message(*step);
            OS << (step_message.empty() ? step->tip : step_message);
          }
        }
      }

      OS << "\n";
    }
  }
}

void writeOptionalReports(const BugReportMgr &mgr) {
  if (!report_options::JsonOutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream json_out(report_options::JsonOutputFile, EC,
                            sys::fs::OF_None);
    if (!EC) {
      mgr.generate_json_report(json_out, report_options::MinConfidenceScore);
      outs() << "\nJSON report written to: " << report_options::JsonOutputFile
             << "\n";
    } else {
      errs() << "Error writing JSON report: " << EC.message() << "\n";
    }
  }

  if (!report_options::SarifOutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream sarif_out(report_options::SarifOutputFile, EC,
                             sys::fs::OF_None);
    if (!EC) {
      mgr.generate_sarif_report(sarif_out, report_options::MinConfidenceScore);
      outs() << "SARIF report written to: " << report_options::SarifOutputFile
             << "\n";
    } else {
      errs() << "Error writing SARIF report: " << EC.message() << "\n";
    }
  }
}
} // namespace

namespace fitx {

// Run all registered FiTx checkers via the legacy pass manager.
void runFiTxAnalysis(Module &M) {
  outs() << "FiTx Bug Finder\n";
  outs() << "================\n\n";
  outs() << "Module: " << M.getName() << "\n";
  outs() << "Functions: " << M.size() << "\n";
  outs() << "Checkers: " << FrameworkPass::passes.size() << "\n";

  auto start = std::chrono::system_clock::now();

  // Ensure LoopInfoWrapperPass is initialized (required by IRGenerator).
  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeLoopInfoWrapperPassPass(Registry);

  legacy::PassManager PM;

  // 1. Build framework IR for all functions (required by FrameworkPass).
  PM.add(new LoopInfoWrapperPass());
  PM.add(new ir_generator::IRGenerator());

  // 2. Run all registered FiTx checkers (e.g. AllDetector runs df, dl, dul,
  //    leak, ref_count, uaf).
  for (fitx::FrameworkPass *P : fitx::FrameworkPass::passes) {
    PM.add(P);
  }

  PM.run(M);

  auto end = std::chrono::system_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  outs() << "Analysis complete.\n";
  if (MeasureAnalysisTime) {
    outs() << "Time: " << duration.count() << " ms\n";
  }
}

} // namespace fitx

int runFiTxCheckerTool(const char *argv0) {
  SMDiagnostic Err;
  LLVMContext Context;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Context);

  if (!M) {
    Err.print(argv0, errs());
    return 1;
  }

  fitx::runFiTxAnalysis(*M);

  BugReportMgr &mgr = BugReportMgr::get_instance();

  if (!report_options::SuppressionFile.empty()) {
    SuppressionManager supp_mgr;
    if (supp_mgr.loadFromFile(report_options::SuppressionFile)) {
      mgr.setSuppressionManager(&supp_mgr);
      mgr.filterSuppressed();
    } else {
      errs() << "Warning: Could not load suppressions from: "
             << report_options::SuppressionFile << "\n";
    }
  }

  mgr.deduplicate_reports(true);
  printDetailedReports(outs(), mgr, Verbose);
  writeOptionalReports(mgr);

  return 0;
}
