// Sifa (Symbolic Interpretation with Fluid Abstractions) verifier tool
// By default uses instruction-by-instruction transfer (no SMT). Use --symabs for SMT-backed analysis.
//  ./build/bin/sifa tmp/sifa-demo/loop.bc --progress  --symabs

#include "Verification/Sifa/Sifa.h"
#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/SymbolicAbstraction/Core/AbstractValue.h"
#include "Verification/SymbolicAbstraction/Utils/PrettyPrinter.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <memory>
#include <string>

using namespace llvm;
using namespace lotus::sifa;

/// Print IntervalState: each value -> [lo, hi] (or top/bottom).
static void printIntervalState(raw_ostream &out, const IntervalState &state) {
  if (state.isBottom()) {
    out << "  (bottom)\n";
    return;
  }
  for (const auto &kv : state.intervals()) {
    const llvm::Value *V = kv.first;
    std::string name = V->getName().str();
    if (name.empty()) {
      raw_string_ostream os(name);
      V->print(os);
      if (name.size() > 40) name = name.substr(0, 37) + "...";
    }
    const Interval &i = kv.second;
    if (i.isBottom())
      out << "  " << name << " : bottom\n";
    else if (i.isTop())
      out << "  " << name << " : [-inf, +inf]\n";
    else {
      out << "  " << name << " : [";
      if (i.lo.hasValue()) out << *i.lo; else out << "-inf";
      out << ", ";
      if (i.hi.hasValue()) out << *i.hi; else out << "+inf";
      out << "]\n";
    }
  }
  for (const auto &kv : state.memory()) {
    const llvm::Value *R = kv.first;
    std::string regName = R->getName().str();
    if (regName.empty()) {
      raw_string_ostream os(regName);
      R->print(os);
      if (regName.size() > 40) regName = regName.substr(0, 37) + "...";
    }
    const Interval &i = kv.second;
    out << "  mem(" << regName << ") : ";
    if (i.isBottom()) out << "bottom\n";
    else if (i.isTop()) out << "[-inf, +inf]\n";
    else
      out << "[" << (i.lo.hasValue() ? std::to_string(*i.lo) : "-inf") << ", "
          << (i.hi.hasValue() ? std::to_string(*i.hi) : "+inf") << "]\n";
  }
}

/// Print OctagonState summary (variable count; with --verbose could print matrix).
static void printOctagonState(raw_ostream &out, const OctagonState &state) {
  if (state.isBottom()) {
    out << "  (bottom)\n";
    return;
  }
  out << "  variables: " << state.varToIndex().size();
  if (!state.memory().empty()) out << ", memory regions: " << state.memory().size();
  out << "\n";
}

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::value_desc("bitcode"));

static cl::opt<std::string> FunctionName(
    "function",
    cl::desc("Function to analyze (default: main or first function)"),
    cl::value_desc("name"));

static cl::opt<std::string> BlockLabel(
    "block",
    cl::desc("Basic block label to analyze to (default: analyze to return)"),
    cl::value_desc("label"));

static cl::opt<std::string> DomainOpt(
    "abstract-domain",
    cl::desc("Abstract domain: 'Interval' (default, no SMT) or 'Octagon'; with --symabs: 'Interval', 'Octagon', or 'Interval, Octagon'"),
    cl::value_desc("domain"),
    cl::init("Interval"));

static cl::opt<bool> UseSymAbs(
    "symabs",
    cl::desc("Use SymbolicAbstraction backend (SMT solver). Default is instruction-by-instruction transfer (no SMT)."),
    cl::init(false));

static cl::opt<bool> ReachabilityOnly(
    "reachability",
    cl::desc("Only check reachability (do not print abstract state)"),
    cl::init(false));

static cl::opt<bool> NoValidateSubset(
    "no-validate-subset",
    cl::desc("Disable validation of LLVM IR subset (may crash on unsupported IR)"),
    cl::init(false));

static cl::opt<bool> ListFunctions(
    "list-functions",
    cl::desc("List all functions in the module"),
    cl::init(false));

static cl::opt<bool> ListBlocks(
    "list-blocks",
    cl::desc("List basic blocks in the selected function"),
    cl::init(false));

static cl::opt<bool> Verbose(
    "verbose",
    cl::desc("Enable verbose output"),
    cl::init(false));

static cl::opt<bool> Progress(
    "progress",
    cl::desc("Print progress messages while analyzing"),
    cl::init(false));

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv,
                              "Sifa - Symbolic Interpretation with Fluid Abstractions on LLVM bitcode\n");

  if (InputFilename.empty()) {
    errs() << "Error: input bitcode file required.\n";
    return 1;
  }

  if (Progress)
    errs() << "[sifa] Loading bitcode: " << InputFilename << "\n";

  LLVMContext context;
  SMDiagnostic err;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);
  if (!module) {
    err.print(argv[0], errs());
    return 1;
  }

  if (Progress)
    errs() << "[sifa] Module loaded\n";

  Function *targetFunc = nullptr;
  if (FunctionName.empty()) {
    targetFunc = module->getFunction("main");
    if (!targetFunc)
      for (auto &F : *module)
        if (!F.isDeclaration()) {
          targetFunc = &F;
          break;
        }
  } else {
    targetFunc = module->getFunction(FunctionName);
  }

  if (!targetFunc) {
    errs() << "Error: Function '"
           << (FunctionName.empty() ? "main" : FunctionName.getValue())
           << "' not found\n";
    return 1;
  }

  if (ListFunctions) {
    outs() << "Functions in module:\n";
    for (auto &F : *module)
      if (!F.isDeclaration())
        outs() << "  " << F.getName() << "\n";
    return 0;
  }

  if (ListBlocks) {
    outs() << "Basic blocks in " << targetFunc->getName() << ":\n";
    for (auto &BB : *targetFunc) {
      if (BB.hasName())
        outs() << "  " << BB.getName() << "\n";
      else
        outs() << "  (unnamed)\n";
    }
    return 0;
  }

  SifaOptions sifaOpts; // for native (instruction transfer, no SMT)

  try {
    if (UseSymAbs) {
      SifaSymAbsOptions options;
      options.abstractDomain = DomainOpt.getValue();
      options.validateLlvmSubset = !NoValidateSubset;
      if (Progress)
        options.progressStream = &errs();

      if (Progress) {
        unsigned nBlocks = 0;
        for (auto &BB : *targetFunc) (void)BB, ++nBlocks;
        errs() << "[sifa] Analyzing (SymAbs/SMT) function '" << targetFunc->getName()
               << "' (" << nBlocks << " blocks, domain: " << options.abstractDomain
               << ")\n";
      }

      if (!BlockLabel.empty()) {
        llvm::BasicBlock *targetBlock = nullptr;
        for (auto &BB : *targetFunc) {
          if (BB.hasName() && BB.getName() == BlockLabel.getValue()) {
            targetBlock = &BB;
            break;
          }
        }
        if (!targetBlock) {
          errs() << "Error: block '" << BlockLabel.getValue()
                 << "' not found in function " << targetFunc->getName() << "\n";
          return 1;
        }

        if (ReachabilityOnly) {
          bool reachable =
              isReachableSymAbs(*module, *targetFunc, *targetBlock, options);
          outs() << "Block " << BlockLabel.getValue()
                 << (reachable ? " is reachable\n" : " is not reachable\n");
          return reachable ? 0 : 1;
        }

        SymAbsState state =
            analyzeSymAbsTo(*module, *targetFunc, *targetBlock, options);
        if (!state) {
          outs() << "Block " << BlockLabel.getValue() << ": unreachable (bottom)\n";
          return 0;
        }
        if (state->isBottom()) {
          outs() << "Block " << BlockLabel.getValue() << ": bottom\n";
          return 0;
        }
        outs() << "Block " << BlockLabel.getValue() << ": state (top/non-bottom)\n";
        if (Verbose && state) {
          symbolic_abstraction::PrettyPrinter pp(/*output_html=*/true);
          state->prettyPrint(pp);
          outs() << pp.str() << "\n";
        }
      } else {
        // Analyze to return (procedure exit)
        if (ReachabilityOnly) {
          SymAbsState state = analyzeSymAbsToReturn(*module, *targetFunc, options);
          bool reachable = state && !state->isBottom();
          outs() << "Return "
                 << (reachable ? "is reachable\n" : "is not reachable\n");
          return reachable ? 0 : 1;
        }

        SymAbsState state = analyzeSymAbsToReturn(*module, *targetFunc, options);
        if (!state) {
          outs() << "Return: unreachable (bottom)\n";
          return 0;
        }
        if (state->isBottom()) {
          outs() << "Return: bottom\n";
          return 0;
        }
        outs() << "Return: state (top/non-bottom)\n";
        if (Verbose && state) {
          symbolic_abstraction::PrettyPrinter pp(/*output_html=*/true);
          state->prettyPrint(pp);
          outs() << pp.str() << "\n";
        }
      }
    } else {
      // Default: native instruction-by-instruction transfer (no SMT)
      std::string domStr = DomainOpt.getValue();
      bool useOctagon = (domStr.find("Octagon") != std::string::npos);

      if (Progress) {
        unsigned nBlocks = 0;
        for (auto &BB : *targetFunc) (void)BB, ++nBlocks;
        errs() << "[sifa] Analyzing (instruction transfer, no SMT) function '"
               << targetFunc->getName() << "' (" << nBlocks << " blocks, domain: "
               << (useOctagon ? "Octagon" : "Interval") << ")\n";
      }

      if (!BlockLabel.empty()) {
        llvm::BasicBlock *targetBlock = nullptr;
        for (auto &BB : *targetFunc) {
          if (BB.hasName() && BB.getName() == BlockLabel.getValue()) {
            targetBlock = &BB;
            break;
          }
        }
        if (!targetBlock) {
          errs() << "Error: block '" << BlockLabel.getValue()
                 << "' not found in function " << targetFunc->getName() << "\n";
          return 1;
        }

        if (useOctagon) {
          OctagonDomain dom(nullptr, nullptr);
          OctagonState initial(false);
          OctagonState state = analyzeToWithOctagonDomain(*targetFunc, *targetBlock, initial, sifaOpts);
          if (dom.isBottom(state)) {
            outs() << "Block " << BlockLabel.getValue() << ": unreachable (bottom)\n";
            return 0;
          }
          if (ReachabilityOnly) {
            outs() << "Block " << BlockLabel.getValue() << " is reachable\n";
            return 0;
          }
          outs() << "Block " << BlockLabel.getValue() << ": state (octagon domain)\n";
          printOctagonState(outs(), state);
        } else {
          IntervalDomain dom(nullptr, nullptr);
          IntervalState initial(false);
          IntervalState state = analyzeToWithIntervalDomain(*targetFunc, *targetBlock, initial, sifaOpts);
          if (dom.isBottom(state)) {
            outs() << "Block " << BlockLabel.getValue() << ": unreachable (bottom)\n";
            return 0;
          }
          if (ReachabilityOnly) {
            outs() << "Block " << BlockLabel.getValue() << " is reachable\n";
            return 0;
          }
          outs() << "Block " << BlockLabel.getValue() << ": state (interval domain)\n";
          printIntervalState(outs(), state);
        }
      } else {
        if (useOctagon) {
          OctagonState initial(false);
          OctagonState state = analyzeToReturnWithOctagonDomain(*targetFunc, initial, sifaOpts);
          OctagonDomain dom(nullptr, nullptr);
          if (dom.isBottom(state)) {
            outs() << "Return: bottom\n";
            return 0;
          }
          if (ReachabilityOnly) {
            outs() << "Return is reachable\n";
            return 0;
          }
          outs() << "Return: state (octagon domain)\n";
          printOctagonState(outs(), state);
        } else {
          IntervalState initial(false);
          IntervalState state = analyzeToReturnWithIntervalDomain(*targetFunc, initial, sifaOpts);
          IntervalDomain dom(nullptr, nullptr);
          if (dom.isBottom(state)) {
            outs() << "Return: bottom\n";
            return 0;
          }
          if (ReachabilityOnly) {
            outs() << "Return is reachable\n";
            return 0;
          }
          outs() << "Return: state (interval domain)\n";
          printIntervalState(outs(), state);
        }
      }
    }
    outs() << "Sifa analysis completed successfully.\n";
  } catch (const std::exception &e) {
    errs() << "Error during analysis: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
