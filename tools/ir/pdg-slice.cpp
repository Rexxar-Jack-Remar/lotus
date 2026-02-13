#include "IR/PDG/Analysis/PropertyBasedSlicing.h"
#include "IR/PDG/Analysis/Slicing.h"
#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace pdg;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<std::string>
    PropertyFile("property-file", cl::desc("Property file (.prp)"),
                 cl::value_desc("filename"), cl::init(""));

static cl::opt<std::string>
    Direction("direction", cl::desc("Slice direction: backward|forward"),
              cl::init("backward"));

static cl::opt<bool> DumpSlice("dump-slice",
                               cl::desc("Dump selected slice nodes"),
                               cl::init(false));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv,
                              "PDG property-based slicing tool\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  PropertySpec spec;
  std::string parseErr;
  if (PropertyFile.empty()) {
    // Symbiotic-compatible default: assertions / unreach-call checks.
    if (!PropertySpec::parseFromString("assertions\n", spec, parseErr)) {
      errs() << "error: " << parseErr << "\n";
      return 1;
    }
  } else {
    if (!PropertySpec::parseFromFile(PropertyFile, spec, parseErr)) {
      errs() << "error: " << parseErr << "\n";
      return 1;
    }
  }

  auto &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);
  initializeTransformUtils(Registry);

  legacy::PassManager PM;
  PM.add(new DataDependencyGraph());
  PM.add(new ControlDependencyGraph());
  PM.add(new ProgramDependencyGraph());
  PM.run(*M);

  ProgramGraph &PDG = ProgramGraph::getInstance();
  PropertyBasedSlicing slicer(PDG);
  PropertyBasedSlicing::NodeSet slice;
  PropertyBasedSlicing::NodeSet criteria = slicer.resolveCriteria(*M, spec);

  if (Direction == "forward")
    slice = slicer.computeForwardSlice(*M, spec);
  else
    slice = slicer.computeBackwardSlice(*M, spec);

  outs() << "property rules: " << spec.rules().size() << "\n";
  outs() << "criteria nodes: " << criteria.size() << "\n";
  outs() << Direction << " slice nodes: " << slice.size() << "\n";

  if (DumpSlice)
    SlicingUtils::printSlice(slice, "Property Slice");

  if (slice.empty()) {
    errs() << "warning: empty slice; criteria might not map to PDG nodes.\n";
  }
  return 0;
}
