#include "Checker/Concurrency/ConcurrencyChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <string>

using namespace llvm;
using namespace concurrency;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input file>"), cl::Required);
static cl::opt<bool> EnableDataRaces("check-data-races", cl::desc("Enable data race detection"), cl::init(true));
static cl::opt<bool> EnableDeadlocks("check-deadlocks", cl::desc("Enable deadlock detection"), cl::init(true));
static cl::opt<bool> EnableAtomicity("check-atomicity", cl::desc("Enable atomicity violation detection"), cl::init(true));
static cl::opt<bool> AnalysisOnly("analysis-only", cl::desc("Run analysis only (no bug checking), dump analysis results"), cl::init(false));
static cl::opt<std::string> AnalysisJsonOutput("analysis-json", cl::desc("Output analysis results as JSON to specified file (requires --analysis-only)"), cl::value_desc("filename"));

int main(int argc, char** argv) {
    // Initialize centralized report options
    report_options::initializeReportOptions();
    
    cl::ParseCommandLineOptions(argc, argv, "Concurrency Checker Tool\n"
                                "  Use --report-json=<file> or --report-sarif=<file> for output\n");

    // Parse the input LLVM IR file
    SMDiagnostic err;
    LLVMContext context;
    std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);

    if (!module) {
        err.print(argv[0], errs());
        return 1;
    }

    outs() << "Analyzing module: " << module->getModuleIdentifier() << "\n";

    // Create the concurrency checker
    ConcurrencyChecker checker(*module);

    // Enable/disable specific checks based on command line options
    checker.enableDataRaceCheck(EnableDataRaces);
    checker.enableDeadlockCheck(EnableDeadlocks);
    checker.enableAtomicityCheck(EnableAtomicity);

    if (AnalysisOnly) {
        // Analysis-only mode: dump analysis results without bug checking
        outs() << "Running concurrency analyses (analysis-only mode)...\n";
        
        if (!AnalysisJsonOutput.empty()) {
            // Output to JSON file
            std::error_code EC;
            raw_fd_ostream json_out(AnalysisJsonOutput, EC, sys::fs::OF_None);
            if (!EC) {
                checker.dumpAnalysisResults(json_out, true);
                outs() << "\nAnalysis results written to JSON: " << AnalysisJsonOutput << "\n";
            } else {
                errs() << "Error writing analysis JSON: " << EC.message() << "\n";
                return 1;
            }
        } else {
            // Output to stdout in human-readable format
            checker.dumpAnalysisResults(outs(), false);
        }
        return 0;
    }

    // Normal mode: Run the checks (bugs are automatically reported to BugReportMgr)
    outs() << "Running concurrency checks...\n";
    checker.runChecks();

    // Print analysis statistics
    auto stats = checker.getStatistics();
    outs() << "\n=== Concurrency Analysis Statistics ===\n";
    outs() << "Total Instructions: " << stats.totalInstructions << "\n";
    outs() << "MHP Pairs: " << stats.mhpPairs << "\n";
    outs() << "Locks Analyzed: " << stats.locksAnalyzed << "\n";
    outs() << "Data Races Found: " << stats.dataRacesFound << "\n";
    outs() << "Deadlocks Found: " << stats.deadlocksFound << "\n";
    outs() << "Atomicity Violations Found: " << stats.atomicityViolationsFound << "\n";

    // Post-processing: Suppression and Deduplication
    BugReportMgr& mgr = BugReportMgr::get_instance();
    
    // 1. Load and apply suppressions
    if (!report_options::SuppressionFile.empty()) {
        SuppressionManager suppMgr;
        if (suppMgr.loadFromFile(report_options::SuppressionFile)) {
            mgr.setSuppressionManager(&suppMgr);
            mgr.filterSuppressed();
            auto stats = suppMgr.getStats();
            outs() << "\nApplied suppressions: " << stats.totalSuppressions 
                   << " across " << stats.totalFiles << " files\n";
        } else {
            errs() << "Warning: Could not load suppressions from: " 
                   << report_options::SuppressionFile << "\n";
        }
    }
    
    // 2. Final deduplication (enhanced algorithm)
    mgr.deduplicate_reports(true);
    
    // 3. Print bug report summary (Clearblue pattern - applies to all checkers)
    mgr.print_summary(outs());
    
    // 4. Handle centralized output formats (applies to all checkers)
    if (!report_options::JsonOutputFile.empty()) {
        std::error_code EC;
        raw_fd_ostream json_out(report_options::JsonOutputFile, EC, sys::fs::OF_None);
        if (!EC) {
            mgr.generate_json_report(json_out, report_options::MinConfidenceScore);
            outs() << "\nJSON report written to: " << report_options::JsonOutputFile << "\n";
        } else {
            errs() << "Error writing JSON report: " << EC.message() << "\n";
        }
    }
    
    // 5. Generate SARIF report if requested
    if (!report_options::SarifOutputFile.empty()) {
        std::error_code EC;
        raw_fd_ostream sarif_out(report_options::SarifOutputFile, EC, sys::fs::OF_None);
        if (!EC) {
            mgr.generate_sarif_report(sarif_out, report_options::MinConfidenceScore);
            outs() << "\nSARIF report written to: " << report_options::SarifOutputFile << "\n";
        } else {
            errs() << "Error writing SARIF report: " << EC.message() << "\n";
        }
    }
    
    size_t total_bugs = stats.dataRacesFound + stats.deadlocksFound + stats.atomicityViolationsFound;
    return total_bugs > 0 ? 1 : 0;
}
