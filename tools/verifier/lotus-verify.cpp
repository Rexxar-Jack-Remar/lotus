#include "Verification/Backend/Backend.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace lotus::verification::backend;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<std::string>
    BackendName("backend",
                cl::desc("Backend: auto|seahorn|sifa|symbolic_abstraction|clam"),
                cl::init("auto"));

static cl::opt<std::string>
    PropertyName("property",
                 cl::desc("Property class: unreach-call|memsafety|overflow|termination"),
                 cl::init("unreach-call"));

static cl::opt<unsigned>
    TimeoutSec("timeout", cl::desc("Timeout in seconds"), cl::init(0));

static cl::list<std::string> ExtraArgs("backend-arg",
                                       cl::desc("Extra backend arg (repeatable)"),
                                       cl::ZeroOrMore);

static cl::opt<bool>
    RunBackend("run", cl::desc("Execute backend command instead of printing"),
               cl::init(false));

static std::string joinCommand(const std::vector<std::string> &cmd) {
  std::string out;
  for (size_t i = 0; i < cmd.size(); ++i) {
    if (i > 0)
      out += " ";
    out += cmd[i];
  }
  return out;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Lotus verifier backend driver\n");

  const PropertyClass prop = parsePropertyClass(PropertyName);
  BackendRegistry &reg = BackendRegistry::instance();

  std::string chosen = BackendName.getValue();
  if (StringRef(BackendName.getValue()).equals_insensitive("auto")) {
    auto rec = reg.recommend(prop);
    if (rec.empty()) {
      errs() << "error: no backend supports property class '" << PropertyName
             << "'\n";
      return 1;
    }
    chosen = rec.front();
  }

  std::unique_ptr<IBackend> backend = reg.create(chosen);
  if (!backend) {
    errs() << "error: unknown backend '" << chosen << "'\n";
    return 1;
  }
  if (!backend->supports(prop)) {
    errs() << "error: backend '" << backend->name()
           << "' does not support property class '" << toString(prop) << "'\n";
    return 1;
  }

  VerificationTask task;
  task.inputBitcode = InputFilename;
  task.property = prop;
  task.timeoutSeconds = TimeoutSec;
  task.extraArgs.assign(ExtraArgs.begin(), ExtraArgs.end());

  const std::vector<std::string> cmd = backend->buildCommand(task);
  outs() << "backend: " << backend->name() << "\n";
  outs() << "property: " << toString(prop) << "\n";
  outs() << "command: " << joinCommand(cmd) << "\n";

  if (!RunBackend)
    return 0;

  if (cmd.empty()) {
    errs() << "error: backend produced empty command\n";
    return 1;
  }

  auto ProgramOrErr = sys::findProgramByName(cmd.front());
  if (!ProgramOrErr) {
    errs() << "error: executable not found for '" << cmd.front() << "'\n";
    return 1;
  }

  SmallVector<StringRef, 16> ArgRefs;
  for (const std::string &arg : cmd)
    ArgRefs.push_back(StringRef(arg));

  std::string Output;
  std::string ErrorOutput;
  Optional<StringRef> Redirects[] = {None, None, None};
  int rc = sys::ExecuteAndWait(*ProgramOrErr, ArgRefs, None, Redirects, 0, 0, &ErrorOutput);
  
  // Combine stdout and stderr for parsing
  Output = ErrorOutput; // In LLVM, ExecuteAndWait captures stderr
  
  VerificationResultInfo result = backend->parseResult(Output, rc);
  
  outs() << "result: " << toString(result.result) << "\n";
  outs() << "message: " << result.message << "\n";
  if (!result.errorTrace.empty()) {
    outs() << "error-trace:\n" << result.errorTrace << "\n";
  }
  outs() << "exit-code: " << result.exitCode << "\n";
  
  // Return 0 for True/Unknown, 1 for False/Error/Timeout
  if (result.result == VerificationResult::True || result.result == VerificationResult::Unknown)
    return 0;
  return 1;
}
