#include "Alias/InclusionBased/CclyzerAA/CclyzerAA.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias::tools;
using namespace lotus::cclyzer;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<AnalysisKind> AnalysisMode(
    "analysis", cl::desc("Choose pointer analysis variant:"),
    cl::values(clEnumValN(AnalysisKind::Subset, "subset",
                          "Subset-based (Andersen) analysis [default]"),
               clEnumValN(AnalysisKind::Unification, "unification",
                          "Unification-based (Steensgaard) analysis"),
               clEnumValN(AnalysisKind::Debug, "debug",
                          "Run both subset and unification")),
    cl::init(AnalysisKind::Subset));

static cl::opt<ContextKind> ContextMode(
    "context-sensitivity",
    cl::desc("Context sensitivity mode:"),
    cl::values(clEnumValN(ContextKind::Insensitive, "insensitive",
                          "Context-insensitive [default]"),
               clEnumValN(ContextKind::CallSite1, "1-cfa", "1-callsite sensitive"),
               clEnumValN(ContextKind::CallSite2, "2-cfa", "2-callsite sensitive"),
               clEnumValN(ContextKind::CallSite3, "3-cfa", "3-callsite sensitive"),
               clEnumValN(ContextKind::Caller1, "1-caller", "1-caller sensitive"),
               clEnumValN(ContextKind::Caller2, "2-caller", "2-caller sensitive")),
    cl::init(ContextKind::Insensitive));

static cl::opt<std::string> SignaturesFile(
    "signatures", cl::desc("File containing function signatures / points-to models"));

static cl::opt<bool> CheckAssertions(
    "check-assertions",
    cl::desc("Verify Datalog consistency assertions"),
    cl::init(false));

static cl::opt<bool> PrintPointsTo(
    "print-pts", cl::desc("Print points-to sets for functions and global variables"),
    cl::init(false));

static cl::opt<bool> PrintCallGraph(
    "print-cg", cl::desc("Print resolved indirect call graph edges"),
    cl::init(false));

static cl::opt<bool> PrintNullPointers(
    "print-nulls", cl::desc("Print values identified as null pointers"),
    cl::init(false));

static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(
      argc, argv,
      "lotus-alias-cclyzer-aa: cclyzer++ Datalog-based Pointer Analysis Tool\n");

  if (!CclyzerAA::isAvailable()) {
    errs() << "Error: CclyzerAA backend is not enabled in this build.\n"
           << "Rebuild Lotus with -DLOTUS_ENABLE_CCLYZER=ON and Soufflé installed.\n";
    return 2;
  }

  LLVMContext Context;
  SMDiagnostic Err;
  auto M = loadIRModule(InputFilename, Context, Err, argv[0]);
  if (!M) {
    return 1;
  }

  if (Verbose) {
    errs() << "Running CclyzerAA on " << M->getName() << " ("
           << M->getFunctionList().size() << " functions)\n";
  }

  CclyzerOptions options;
  options.analysis = AnalysisMode.getValue();
  options.context = ContextMode.getValue();
  options.signaturesPath = SignaturesFile.getValue();
  options.checkAssertions = CheckAssertions.getValue();

  CclyzerAA aa(options);
  if (!aa.run(*M)) {
    errs() << "Error: CclyzerAA analysis execution failed.\n";
    return 1;
  }

  outs() << "CclyzerAA completed successfully.\n";

  if (PrintPointsTo) {
    outs() << "\n=== Points-To Sets ===\n";
    for (const auto &G : M->globals()) {
      std::vector<const Value *> pts;
      if (aa.getPointsToSet(&G, pts)) {
        outs() << "Global " << G.getName() << " points to " << pts.size()
               << " target(s):\n";
        for (const auto *target : pts) {
          outs() << "  -> " << *target << "\n";
        }
      }
    }
    for (const auto &F : *M) {
      for (const auto &BB : F) {
        for (const auto &I : BB) {
          if (I.getType()->isPointerTy()) {
            std::vector<const Value *> pts;
            if (aa.getPointsToSet(&I, pts)) {
              outs() << "Instruction " << I << " points to " << pts.size()
                     << " target(s)\n";
            }
          }
        }
      }
    }
  }

  if (PrintCallGraph) {
    outs() << "\n=== Indirect Call Targets ===\n";
    unsigned totalIndirect = 0;
    unsigned resolvedIndirect = 0;
    for (const auto &F : *M) {
      for (const auto &BB : F) {
        for (const auto &I : BB) {
          if (const auto *CB = dyn_cast<CallBase>(&I)) {
            if (CB->isIndirectCall()) {
              totalIndirect++;
              std::vector<const Value *> targets;
              if (aa.getIndirectCallTargets(CB, targets)) {
                resolvedIndirect++;
                outs() << "Call site: " << *CB << "\n";
                for (const auto *t : targets) {
                  outs() << "  -> " << t->getName() << "\n";
                }
              }
            }
          }
        }
      }
    }
    outs() << "Total indirect calls: " << totalIndirect
           << ", resolved: " << resolvedIndirect << "\n";
  }

  if (PrintNullPointers) {
    std::set<const Value *> nulls;
    if (aa.getNullPtrSet(nulls)) {
      outs() << "\n=== Identified Null Pointers (" << nulls.size() << ") ===\n";
      for (const auto *v : nulls) {
        outs() << "  " << *v << "\n";
      }
    }
  }

  return 0;
}
