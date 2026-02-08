/*
 * lotus-dfa-diff
 *
 * Differential testing for lib/Dataflow engines. Runs overlapping analyses
 * (Elimination, Mono, optionally WPDS) on the same LLVM bitcode and dumps
 * results in a canonical format so that outputs can be diffed. Used with
 * random C → bitcode to find discrepancies between engines.
 *
 * Usage:
 *   lotus-dfa-diff [options] <bitcode>
 *   --analysis=liveness   (default) Compare liveness analysis
 *   --out-dir=DIR        Write engine outputs to DIR/elim.txt, DIR/mono.txt
 *   --engine=elim|mono|both   Run only selected engine(s) (default: both)
 */

#include "Dataflow/Elimination/EliminationPasses.h"
#include "Dataflow/Mono/Analyses/Intraprocedural/LiveVariablesAnalysis.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                           cl::Required);
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory for engine dumps"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<std::string> AnalysisOpt("analysis",
                                        cl::desc("Analysis to compare: liveness (default)"),
                                        cl::init("liveness"));
static cl::opt<std::string> EngineOpt("engine",
                                      cl::desc("Engine(s) to run: elim, mono, both"),
                                      cl::init("both"));

namespace {

// Build a stable Value -> id map for a function (args then instructions in order).
void buildValueIds(llvm::Function *F,
                   std::unordered_map<const llvm::Value *, std::string> &ValueToId,
                   std::vector<llvm::Instruction *> &OrderedInsts) {
  ValueToId.clear();
  OrderedInsts.clear();
  unsigned ArgIdx = 0;
  for (auto &Arg : F->args()) {
    ValueToId[&Arg] = "arg" + std::to_string(ArgIdx++);
  }
  unsigned InstIdx = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      OrderedInsts.push_back(&I);
      ValueToId[&I] = "i" + std::to_string(InstIdx++);
    }
  }
}

// Format a set of Values as sorted comma-separated ids.
void formatSet(llvm::raw_ostream &OS,
               const std::set<llvm::Value *> &S,
               const std::unordered_map<const llvm::Value *, std::string> &ValueToId) {
  std::vector<std::string> ids;
  for (llvm::Value *V : S) {
    auto It = ValueToId.find(V);
    if (It != ValueToId.end())
      ids.push_back(It->second);
  }
  std::sort(ids.begin(), ids.end());
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) OS << ",";
    OS << ids[i];
  }
}

void formatSetConst(llvm::raw_ostream &OS,
                    const std::set<const llvm::Value *> &S,
                    const std::unordered_map<const llvm::Value *, std::string> &ValueToId) {
  std::vector<std::string> ids;
  for (const llvm::Value *V : S) {
    auto It = ValueToId.find(V);
    if (It != ValueToId.end())
      ids.push_back(It->second);
  }
  std::sort(ids.begin(), ids.end());
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) OS << ",";
    OS << ids[i];
  }
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Dataflow engine diff testing\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  // Prepare IR: mem2reg + instnamer for stable SSA and naming.
  legacy::PassManager PM;
  PM.add(createPromoteMemoryToRegisterPass());
  PM.add(createInstructionNamerPass());
  PM.run(*M);

  const bool runElim = (EngineOpt == "elim" || EngineOpt == "both");
  const bool runMono = (EngineOpt == "mono" || EngineOpt == "both");
  if (!runElim && !runMono) {
    errs() << "error: --engine must be elim, mono, or both\n";
    return 1;
  }

  if (AnalysisOpt != "liveness") {
    errs() << "error: only --analysis=liveness is supported currently\n";
    return 1;
  }

  std::unique_ptr<raw_fd_ostream> ElimOS;
  std::unique_ptr<raw_fd_ostream> MonoOS;
  if (!OutDir.empty()) {
    std::error_code EC;
    ElimOS = std::make_unique<raw_fd_ostream>(OutDir + "/elim.txt", EC);
    if (EC) {
      errs() << "error: cannot create " << OutDir << "/elim.txt: " << EC.message() << "\n";
      return 1;
    }
    MonoOS = std::make_unique<raw_fd_ostream>(OutDir + "/mono.txt", EC);
    if (EC) {
      errs() << "error: cannot create " << OutDir << "/mono.txt: " << EC.message() << "\n";
      return 1;
    }
  }
  raw_ostream &ElimOut = ElimOS ? *ElimOS : outs();
  raw_ostream &MonoOut = MonoOS ? *MonoOS : outs();

  if (runElim) {
    legacy::FunctionPassManager FPM(M.get());
    auto *ElimPass = new elimination::ElimLiveVariablesPass();
    FPM.add(ElimPass);
    bool firstElim = true;
    for (auto &F : *M) {
      if (F.isDeclaration())
        continue;
      FPM.run(F);
      std::unordered_map<const llvm::Value *, std::string> ValueToId;
      std::vector<llvm::Instruction *> OrderedInsts;
      buildValueIds(&F, ValueToId, OrderedInsts);
      if (OutDir.empty() && firstElim) {
        ElimOut << "[elim]\n";
        firstElim = false;
      }
      const auto &Res = ElimPass->getResult();
      ElimOut << "FUNC " << F.getName().str() << "\n";
      for (llvm::Instruction *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        ElimOut << " inst_" << ValueToId.at(I) << " IN: ";
        formatSetConst(ElimOut, InSet, ValueToId);
        ElimOut << "\n";
      }
    }
  }

  if (runMono) {
    bool firstMono = true;
    for (auto &F : *M) {
      if (F.isDeclaration())
        continue;
      std::unordered_map<const llvm::Value *, std::string> ValueToId;
      std::vector<llvm::Instruction *> OrderedInsts;
      buildValueIds(&F, ValueToId, OrderedInsts);
      std::unique_ptr<mono::DataFlowResult> MonoRes = mono::runLiveVariablesAnalysis(&F);
      if (OutDir.empty() && firstMono) {
        MonoOut << "[mono]\n";
        firstMono = false;
      }
      MonoOut << "FUNC " << F.getName().str() << "\n";
      if (MonoRes) {
        for (llvm::Instruction *I : OrderedInsts) {
          const auto &InSet = MonoRes->IN(I);
          MonoOut << " inst_" << ValueToId.at(I) << " IN: ";
          formatSet(MonoOut, InSet, ValueToId);
          MonoOut << "\n";
        }
      } else {
        for (llvm::Instruction *I : OrderedInsts)
          MonoOut << " inst_" << ValueToId.at(I) << " IN: \n";
      }
    }
  }

  if (ElimOS)
    ElimOS->close();
  if (MonoOS)
    MonoOS->close();

  return 0;
}
