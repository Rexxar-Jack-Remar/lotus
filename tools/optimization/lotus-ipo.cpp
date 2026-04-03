/*
 * lotus-ipo
 * A command-line driver for inter-procedural optimizations in
 * lib/Optimization/IPO.
 */

#include "Alias/seadsa/AllocSiteInfo.hh"
#include "Alias/seadsa/AllocWrapInfo.hh"
#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/DsaLibFuncInfo.hh"
#include "Alias/seadsa/InitializePasses.hh"
#include "Alias/seadsa/ShadowMem.hh"
#include "Alias/seadsa/support/RemovePtrToInt.hh"

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
#include <llvm/Transforms/IPO.h>

using namespace llvm;

namespace {

static cl::OptionCategory OptCat("Lotus IPO Optimization Tool");

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required, cl::cat(OptCat));

static cl::opt<std::string>
    OutputFilename("o", cl::desc("Override output filename (default: -)"),
                   cl::value_desc("filename"), cl::init("-"), cl::cat(OptCat));

static cl::opt<bool>
    OutputAssembly("S", cl::desc("Write LLVM assembly instead of bitcode"),
                   cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableAllIP("ip-all", cl::desc("Enable all inter-procedural IPO passes"),
                cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableIPDSE("ipdse",
                cl::desc("Run inter-procedural dead store elimination"),
                cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableIPRLE("ip-rle",
                cl::desc("Run inter-procedural redundant load elimination"),
                cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableIPSink("ip-sink", cl::desc("Run inter-procedural store sinking"),
                 cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableIPForward("ip-forward",
                    cl::desc("Run inter-procedural store-to-load forwarding"),
                    cl::init(false), cl::cat(OptCat));

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

static void addMemorySSAPrerequisites(legacy::PassManager &PM) {
  PM.add(new seadsa::RemovePtrToInt());
  PM.add(new seadsa::AllocWrapInfo());
  PM.add(new seadsa::DsaLibFuncInfo());
  PM.add(new seadsa::AllocSiteInfo());
  PM.add(new seadsa::DsaAnalysis());
  PM.add(seadsa::createShadowMemPass());
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);
  initializeTransformUtils(Registry);
  initializeIPO(Registry);
  initializeCallGraphWrapperPassPass(Registry);
  initializeGlobalsAAWrapperPassPass(Registry);
  initializeTargetLibraryInfoWrapperPassPass(Registry);
  initializeDominatorTreeWrapperPassPass(Registry);
  initializeAssumptionCacheTrackerPass(Registry);

  seadsa::initializeAnalysisPasses(Registry);
  initializeRemovePtrToIntPass(Registry);
  initializeAllocWrapInfoPass(Registry);
  initializeDsaLibFuncInfoPass(Registry);
  initializeAllocSiteInfoPass(Registry);
  initializeDsaAnalysisPass(Registry);
  initializeShadowMemPassPass(Registry);

  cl::ParseCommandLineOptions(
      argc, argv, "Lotus optimization tool for inter-procedural IPO passes\n");

  if (EnableAllIP) {
    EnableIPDSE = true;
    EnableIPRLE = true;
    EnableIPSink = true;
    EnableIPForward = true;
  }

  if (!EnableIPDSE && !EnableIPRLE && !EnableIPSink && !EnableIPForward) {
    errs()
        << "error: no IPO pass selected; use -ip-all or a specific IPO flag\n";
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
  addMemorySSAPrerequisites(PM);

  bool Ok = true;
  if (EnableIPDSE)
    Ok &= addPassByName(PM, "ipdse");
  if (EnableIPRLE)
    Ok &= addPassByName(PM, "ip-rle");
  if (EnableIPSink)
    Ok &= addPassByName(PM, "ip-sink");
  if (EnableIPForward)
    Ok &= addPassByName(PM, "ip-forward");

  if (!Ok)
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
