#include "Dataflow/NPA/Tooling/NPABooleanDriver.h"
#include "Dataflow/NPA/Tooling/NPALLVMDriver.h"

#include "Dataflow/Tooling/ToolSupport.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <system_error>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input>"),
                                          cl::Required);
static cl::opt<std::string> InputFormat(
    "input-format", cl::desc("Input format: llvm (default), boolean"),
    cl::init("llvm"));
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<bool> StdoutOpt(
    "stdout", cl::desc("Write results to stdout when --out-dir is not set"),
    cl::init(false));
static cl::opt<bool> PrintBlockResultsOpt(
    "print-block-results", cl::desc("Print per-block or per-node facts"),
    cl::init(false));
static cl::opt<std::string> AnalysisOpt(
    "analysis", cl::desc("LLVM analysis, or predicate for Boolean programs"),
    cl::init("liveness"));
static cl::opt<std::string> SolverOpt(
    "solver", cl::desc("Solver: newton (default), kleene"),
    cl::init("newton"));
static cl::opt<std::string> LinearSolverOpt(
    "linear-solver", cl::desc("Newton linear solver: scc, adaptive_scc, tensor"),
    cl::init("scc"));
static cl::opt<std::string> NewtonRoundOpt(
    "newton-round", cl::desc("Newton round: dense, static, always_maybe, sparse"),
    cl::init("dense"));
static cl::opt<std::string> BooleanEntryOpt(
    "bp-entry", cl::desc("Boolean-program entry procedure"), cl::init("main"));
static cl::opt<std::string> BooleanQueryOpt(
    "bp-query", cl::desc("Boolean label query: <procedure>:<label>"),
    cl::init(""));
static cl::opt<bool> RequireTensorOpt(
    "require-tensor", cl::desc("Fail if no tensor round runs"), cl::init(false));
static cl::opt<bool> BooleanPrepareOnlyOpt(
    "bp-prepare-only", cl::desc("Parse and build Boolean NPA equations"),
    cl::init(false));
static cl::opt<bool> VerifySolversOpt(
    "verify-solvers", cl::desc("Compare Kleene, SCC Newton, and tensor Newton"),
    cl::init(false));

int main(int argc, char **argv) {
  InitLLVM init(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "NPA analysis driver\n");

  if (InputFormat != "llvm" && InputFormat != "boolean") {
    errs() << "error: unknown input format '" << InputFormat << "'\n";
    return 1;
  }
  if (SolverOpt != "newton" && SolverOpt != "kleene") {
    errs() << "error: unknown NPA solver '" << SolverOpt << "'\n";
    return 1;
  }
  if (LinearSolverOpt != "scc" && LinearSolverOpt != "adaptive_scc" &&
      LinearSolverOpt != "tensor") {
    errs() << "error: unknown NPA linear solver '" << LinearSolverOpt << "'\n";
    return 1;
  }
  if (NewtonRoundOpt != "dense" && NewtonRoundOpt != "static" &&
      NewtonRoundOpt != "always_maybe" && NewtonRoundOpt != "sparse") {
    errs() << "error: unknown Newton round strategy '" << NewtonRoundOpt
           << "'\n";
    return 1;
  }
  if (SolverOpt == "kleene" && NewtonRoundOpt != "dense") {
    errs() << "error: --newton-round applies only to --solver=newton\n";
    return 1;
  }
  if (RequireTensorOpt &&
      (SolverOpt != "newton" || LinearSolverOpt != "tensor")) {
    errs() << "error: --require-tensor needs --solver=newton "
              "--linear-solver=tensor\n";
    return 1;
  }

  raw_null_ostream null_os;
  std::unique_ptr<raw_fd_ostream> file_os;
  std::error_code ec;
  raw_ostream &os = lotus::dataflow_tool::selectOutputStream(
      StdoutOpt, OutDir, "npa.txt", file_os, null_os, ec);
  if (ec) {
    errs() << "error: cannot create " << OutDir << "/npa.txt: "
           << ec.message() << "\n";
    return 1;
  }

  if (InputFormat == "boolean") {
    if (AnalysisOpt.getNumOccurrences() && AnalysisOpt != "predicate") {
      errs() << "error: Boolean programs require --analysis=predicate\n";
      return 1;
    }
    return runNpaBoolean(InputFilename, os, SolverOpt, LinearSolverOpt,
                         NewtonRoundOpt, BooleanEntryOpt, BooleanQueryOpt,
                         PrintBlockResultsOpt,
                         RequireTensorOpt, BooleanPrepareOnlyOpt,
                         VerifySolversOpt);
  }
  if (BooleanPrepareOnlyOpt) {
    errs() << "error: --bp-prepare-only requires Boolean input\n";
    return 1;
  }
  if (!BooleanQueryOpt.empty()) {
    errs() << "error: --bp-query requires Boolean input\n";
    return 1;
  }
  if (VerifySolversOpt) {
    errs() << "error: --verify-solvers requires Boolean input\n";
    return 1;
  }
  if (RequireTensorOpt) {
    errs() << "error: --require-tensor currently applies to Boolean input\n";
    return 1;
  }
  return runNpaLLVM(InputFilename, os, AnalysisOpt, SolverOpt, LinearSolverOpt,
                    NewtonRoundOpt, PrintBlockResultsOpt,
                    NewtonRoundOpt.getNumOccurrences() != 0, argv[0]);
}
