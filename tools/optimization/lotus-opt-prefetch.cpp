/*
 * lotus-prefetch
 * A command-line driver for software prefetching passes in
 * lib/Optimization/Prefetch.
 */

#include "Optimization/Prefetch/PrefetchHints.h"

#include <memory>
#include <string>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace {

static cl::OptionCategory OptCat("Lotus Prefetch Optimization Tool");

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required, cl::cat(OptCat));

static cl::opt<std::string>
    OutputFilename("o", cl::desc("Override output filename (default: -)"),
                   cl::value_desc("filename"), cl::init("-"), cl::cat(OptCat));

static cl::opt<bool>
    OutputAssembly("S", cl::desc("Write LLVM assembly instead of bitcode"),
                   cl::init(false), cl::cat(OptCat));

static cl::opt<std::string> ProfileFilename(
    "profile",
    cl::desc("Sample-profile input used by profile-guided prefetching"),
    cl::value_desc("filename"), cl::init(""), cl::cat(OptCat));

static bool addPassByName(legacy::PassManager &PM, StringRef PassName) {
  const PassRegistry &Registry = *PassRegistry::getPassRegistry();
  const PassInfo *PI = Registry.getPassInfo(PassName);
  if (!PI) {
    errs() << "error: unknown pass '" << PassName << "'\n";
    return false;
  }
  PM.add(PI->createPass());
  return true;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);
  initializeTransformUtils(Registry);
  initializeScalarOpts(Registry);

  cl::ParseCommandLineOptions(
      argc, argv, "Lotus optimization tool for software prefetching passes\n");

  if (!ProfileFilename.empty())
    PrefetchFile.setValue(ProfileFilename);

  if (PrefetchDistanceProviderMode == PrefetchDistanceProvider::Profile &&
      PrefetchFile.empty()) {
    errs() << "error: profile-guided prefetching requires --profile=<file> "
              "(or legacy --input-file=<file>)\n";
    return 1;
  }

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  if (verifyModule(*M, &errs())) {
    errs() << "error: module verification failed\n";
    return 1;
  }

  std::unique_ptr<ToolOutputFile> Out;
  if (!OutputFilename.empty() && OutputFilename != "-") {
    std::error_code EC;
    Out =
        std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_None);
    if (EC) {
      errs() << EC.message() << '\n';
      return 1;
    }
  }

  legacy::PassManager PM;
  if (!addPassByName(PM, "SWPrefetchingLLVMPass"))
    return 1;

  PM.run(*M);

  raw_ostream &OS = Out ? Out->os() : outs();
  if (OutputAssembly) {
    M->print(OS, nullptr);
  } else {
    WriteBitcodeToFile(*M, OS);
  }

  if (Out)
    Out->keep();

  return 0;
}
