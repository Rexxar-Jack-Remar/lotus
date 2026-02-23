/*
 * lotus-dfa-elim
 *
 * Dataflow testing tool: Elimination engine.
 */

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

#include "Dataflow/APA/Analyses/Intra/IntraAvailableExpressions.h"
#include "Dataflow/APA/Analyses/Intra/IntraConstantPropagation.h"
#include "Dataflow/APA/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/APA/Analyses/Intra/IntraReachable.h"
#include "Dataflow/APA/Analyses/Intra/IntraReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Intra/IntraUninitVariables.h"

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                          cl::Required);
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<std::string> AnalysisOpt(
    "analysis",
    cl::desc("Analysis: liveness (default), reaching_defs, uninitialized, "
             "constant_prop, available_exprs, reachable"),
    cl::init("liveness"));

namespace {

void buildValueIds(
    llvm::Function *F,
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

template <typename T>
void formatValueSet(
    raw_ostream &OS, const std::set<T> &S,
    const std::unordered_map<const llvm::Value *, std::string> &ValueToId) {
  std::vector<std::string> ids;
  for (const llvm::Value *V : S) {
    auto It = ValueToId.find(V);
    if (It != ValueToId.end())
      ids.push_back(It->second);
  }
  std::sort(ids.begin(), ids.end());
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i)
      OS << ",";
    OS << ids[i];
  }
}

std::string formatExpressionKey(const elimination::ExpressionKey &Key) {
  std::ostringstream ss;
  ss << "op" << Key.Opcode << "(";
  for (size_t i = 0; i < Key.Ops.size(); ++i) {
    if (i)
      ss << ",";
    ss << Key.Ops[i];
  }
  ss << ")";
  return ss.str();
}

std::string formatValueLatticeElement(const llvm::ValueLatticeElement &Val) {
  std::ostringstream ss;
  if (Val.isUndef())
    ss << "undef";
  else if (Val.isUnknown())
    ss << "unknown";
  else if (Val.isOverdefined())
    ss << "overdefined";
  else if (Val.isNotConstant())
    ss << "notconst";
  else if (Val.isConstant()) {
    if (auto *CI = dyn_cast<ConstantInt>(Val.getConstant()))
      ss << "const" << CI->getZExtValue();
    else
      ss << "const";
  } else
    ss << "lattice";
  return ss.str();
}

template <typename ValueType>
void formatConstPropMap(
    raw_ostream &OS,
    const std::unordered_map<const llvm::Value *, ValueType> &M,
    const std::unordered_map<const llvm::Value *, std::string> &ValueToId) {
  std::vector<std::string> entries;
  for (const auto &p : M) {
    std::ostringstream ss;
    auto It = ValueToId.find(p.first);
    ss << (It != ValueToId.end() ? It->second : "v") << "=";
    ss << formatValueLatticeElement(p.second);
    entries.push_back(ss.str());
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i)
      OS << ",";
    OS << entries[i];
  }
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Elimination engine testing\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  legacy::PassManager PM;
  PM.add(createPromoteMemoryToRegisterPass());
  PM.add(createInstructionNamerPass());
  PM.run(*M);

  raw_ostream *OutOS = &outs();
  std::unique_ptr<raw_fd_ostream> FileOS;
  if (!OutDir.empty()) {
    std::error_code EC;
    FileOS = std::make_unique<raw_fd_ostream>(OutDir + "/elim.txt", EC);
    if (EC) {
      errs() << "error: cannot create " << OutDir
             << "/elim.txt: " << EC.message() << "\n";
      return 1;
    }
    OutOS = FileOS.get();
  }
  raw_ostream &OS = *OutOS;

  OS << "[elim:" << AnalysisOpt << "]\n";

  for (auto &F : *M) {
    if (F.isDeclaration())
      continue;

    std::unordered_map<const llvm::Value *, std::string> ValueToId;
    std::vector<llvm::Instruction *> OrderedInsts;
    buildValueIds(&F, ValueToId, OrderedInsts);

    OS << "FUNC " << F.getName().str() << "\n";

    if (AnalysisOpt == "liveness") {
      auto Res = elimination::runIntraElimLiveVariables(&F);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, InSet, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisOpt == "reaching_defs") {
      auto Res = elimination::runIntraElimReachingDefinitions(&F, nullptr);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, InSet, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisOpt == "uninitialized") {
      auto Res = elimination::runIntraElimUninitVariables(&F, nullptr);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, InSet, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisOpt == "constant_prop") {
      auto Res = elimination::runIntraElimConstantPropagation(&F, nullptr);
      for (auto *I : OrderedInsts) {
        const auto &InMap = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatConstPropMap(OS, InMap, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisOpt == "available_exprs") {
      auto Res = elimination::runIntraElimAvailableExpressions(&F, nullptr);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        std::vector<std::string> exprs;
        exprs.reserve(InSet.size());
        for (const auto &expr : InSet) {
          exprs.push_back(formatExpressionKey(expr));
        }
        std::sort(exprs.begin(), exprs.end());
        for (size_t i = 0; i < exprs.size(); ++i) {
          if (i)
            OS << ",";
          OS << exprs[i];
        }
        OS << "\n";
      }
    } else if (AnalysisOpt == "reachable") {
      auto Res = elimination::runIntraElimReachable(&F);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        OS << (InSet ? "true" : "false");
        OS << "\n";
      }
    }
  }

  return 0;
}
