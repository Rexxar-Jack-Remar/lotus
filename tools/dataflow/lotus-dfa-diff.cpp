/*
 * lotus-dfa-diff
 *
 * Differential testing for lib/Dataflow engines. Runs analyses using
 * different engines (Elimination, Mono, IFDS) on the same LLVM bitcode
 * and dumps results in a canonical format for diff-based comparison.
 *
 * Usage:
 *   lotus-dfa-diff [options] <bitcode>
 *
 * Options:
 *   --analysis=NAME       Analysis to run (default: liveness)
 *   --engine=NAME        Engine(s): elim, mono, ifds, all (default: all)
 *   --out-dir=DIR        Write outputs to DIR/{engine}.txt
 *   --verbose            Enable verbose output
 *
 * Available analyses:
 *   liveness              Live variables (SSA form)
 *   reaching_defs         Reaching definitions
 *   uninitialized          Uninitialized variables
 *   constant_prop         Constant propagation
 *   available_exprs       Available expressions
 *   reachable             Reachable analysis
 *
 * Example:
 *   lotus-dfa-diff --analysis=liveness --engine=all --out-dir=/tmp/out
 * program.bc diff /tmp/out/elim.txt /tmp/out/mono.txt
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
#include "Dataflow/IFDS/Clients/IFDSReachingDefinitions.h"
#include "Dataflow/IFDS/Clients/IFDSUninitializedVariables.h"
#include "Dataflow/IFDS/Solvers/IFDSSolver.h"
#include "Dataflow/Mono/Analyses/Intra/IntraConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/Mono/Analyses/Intra/IntraReachable.h"
#include "Dataflow/Mono/Analyses/Intra/IntraUninitVariables.h"

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
static cl::opt<std::string>
    EngineOpt("engine",
              cl::desc("Engine(s): elim, mono, ifds, all (default: all)"),
              cl::init("all"));
static cl::opt<bool> Verbose("verbose", cl::desc("Enable verbose output"),
                             cl::init(false));

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

std::string formatMonoConstantValue(const mono::ConstantPropagationValue &Val) {
  std::ostringstream ss;
  switch (Val.Tag) {
  case mono::ConstantPropagationTag::Top:
    ss << "top";
    break;
  case mono::ConstantPropagationTag::Const:
    ss << "const" << Val.ConstValue;
    break;
  case mono::ConstantPropagationTag::Bottom:
    ss << "bottom";
    break;
  }
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
    ss << formatMonoConstantValue(p.second);
    entries.push_back(ss.str());
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i)
      OS << ",";
    OS << entries[i];
  }
}

template <>
void formatConstPropMap<llvm::ValueLatticeElement>(
    raw_ostream &OS,
    const std::unordered_map<const llvm::Value *, llvm::ValueLatticeElement> &M,
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

std::string formatIFDSFactToString(
    const ifds::DefinitionFact &fact,
    const std::unordered_map<const llvm::Value *, std::string> &ValueToId) {
  if (fact.is_zero()) {
    return "zero";
  }
  std::ostringstream ss;
  auto varIt = ValueToId.find(fact.get_variable());
  auto defIt = ValueToId.find(fact.get_definition_site());
  ss << "def(" << (varIt != ValueToId.end() ? varIt->second : "v") << ","
     << (defIt != ValueToId.end() ? defIt->second : "i") << ")";
  return ss.str();
}

std::string formatIFDSFactToString(
    const ifds::UninitVarFact &fact,
    const std::unordered_map<const llvm::Value *, std::string> &ValueToId) {
  if (fact.is_zero()) {
    return "zero";
  }
  std::ostringstream ss;
  auto It = ValueToId.find(fact.value);
  ss << (fact.is_uninitialized() ? "uninit(" : "init(")
     << (It != ValueToId.end() ? It->second : "v") << ")";
  return ss.str();
}

template <typename Fact>
void formatIFDSFactSet(
    raw_ostream &OS, const std::set<Fact> &facts,
    const std::unordered_map<const llvm::Value *, std::string> &ValueToId) {
  std::vector<std::string> formatted;
  formatted.reserve(facts.size());
  for (const auto &fact : facts) {
    formatted.push_back(formatIFDSFactToString(fact, ValueToId));
  }
  std::sort(formatted.begin(), formatted.end());
  for (size_t i = 0; i < formatted.size(); ++i) {
    if (i)
      OS << ",";
    OS << formatted[i];
  }
}

std::string getOutputPath(const std::string &Dir, const std::string &Engine) {
  if (Dir.empty())
    return "";
  return Dir + "/" + Engine + ".txt";
}

class OutputManager {
  std::unique_ptr<raw_fd_ostream> elim_out;
  std::unique_ptr<raw_fd_ostream> mono_out;
  std::unique_ptr<raw_fd_ostream> ifds_out;

public:
  raw_ostream &getStream(const std::string &Engine) {
    if (Engine == "elim") {
      if (!elim_out) {
        std::error_code EC;
        elim_out =
            std::make_unique<raw_fd_ostream>(getOutputPath(OutDir, "elim"), EC);
        if (EC)
          errs() << "warning: cannot create elim.txt: " << EC.message() << "\n";
      }
      return *elim_out;
    } else if (Engine == "mono") {
      if (!mono_out) {
        std::error_code EC;
        mono_out =
            std::make_unique<raw_fd_ostream>(getOutputPath(OutDir, "mono"), EC);
        if (EC)
          errs() << "warning: cannot create mono.txt: " << EC.message() << "\n";
      }
      return *mono_out;
    } else if (Engine == "ifds") {
      if (!ifds_out) {
        std::error_code EC;
        ifds_out =
            std::make_unique<raw_fd_ostream>(getOutputPath(OutDir, "ifds"), EC);
        if (EC)
          errs() << "warning: cannot create ifds.txt: " << EC.message() << "\n";
      }
      return *ifds_out;
    }
    return outs();
  }

  void close() {
    if (elim_out)
      elim_out->close();
    if (mono_out)
      mono_out->close();
    if (ifds_out)
      ifds_out->close();
  }
};

void runEliminationAnalysis(Module &M, const std::string &AnalysisName,
                            OutputManager &OutMgr,
                            const std::string &EngineName, AAResults *AA) {
  raw_ostream &OS = OutMgr.getStream(EngineName);
  bool firstFunc = true;

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;

    std::unordered_map<const llvm::Value *, std::string> ValueToId;
    std::vector<llvm::Instruction *> OrderedInsts;
    buildValueIds(&F, ValueToId, OrderedInsts);

    if (OutDir.empty() && firstFunc) {
      OS << "[" << EngineName << ":" << AnalysisName << "]\n";
      firstFunc = false;
    }

    OS << "FUNC " << F.getName().str() << "\n";

    if (AnalysisName == "liveness") {
      auto Res = elimination::runIntraElimLiveVariables(&F);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, InSet, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "reaching_defs") {
      auto Res = elimination::runIntraElimReachingDefinitions(&F, AA);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, InSet, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "uninitialized") {
      auto Res = elimination::runIntraElimUninitVariables(&F, AA);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatValueSet(OS, InSet, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "constant_prop") {
      auto Res = elimination::runIntraElimConstantPropagation(&F, AA);
      for (auto *I : OrderedInsts) {
        const auto &InMap = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        formatConstPropMap(OS, InMap, ValueToId);
        OS << "\n";
      }
    } else if (AnalysisName == "available_exprs") {
      auto Res = elimination::runIntraElimAvailableExpressions(&F, AA);
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
    } else if (AnalysisName == "reachable") {
      auto Res = elimination::runIntraElimReachable(&F);
      for (auto *I : OrderedInsts) {
        const auto &InSet = Res.IN(I);
        OS << "  " << ValueToId.at(I) << " IN: ";
        OS << (InSet ? "true" : "false");
        OS << "\n";
      }
    }
  }
}

void runMonoAnalysis(Module &M, const std::string &AnalysisName,
                     OutputManager &OutMgr, const std::string &EngineName) {
  raw_ostream &OS = OutMgr.getStream(EngineName);
  bool firstFunc = true;

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;

    std::unordered_map<const llvm::Value *, std::string> ValueToId;
    std::vector<llvm::Instruction *> OrderedInsts;
    buildValueIds(&F, ValueToId, OrderedInsts);

    if (OutDir.empty() && firstFunc) {
      OS << "[" << EngineName << ":" << AnalysisName << "]\n";
      firstFunc = false;
    }

    OS << "FUNC " << F.getName().str() << "\n";

    if (AnalysisName == "liveness") {
      auto Res = mono::runLiveVariablesAnalysis(&F);
      if (Res) {
        for (auto *I : OrderedInsts) {
          const auto &InSet = Res->IN(I);
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, InSet, ValueToId);
          OS << "\n";
        }
      }
    } else if (AnalysisName == "reachable") {
      auto Res = mono::runReachableAnalysis(&F);
      if (Res) {
        for (auto *I : OrderedInsts) {
          const auto &InSet = Res->IN(I);
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, InSet, ValueToId);
          OS << "\n";
        }
      }
    } else if (AnalysisName == "constant_prop") {
      auto Res = mono::runIntraMonoConstantPropagation(&F);
      for (auto *I : OrderedInsts) {
        OS << "  " << ValueToId.at(I) << " IN: ";
        auto It = Res.find(I);
        if (It != Res.end()) {
          formatConstPropMap(OS, It->second, ValueToId);
        }
        OS << "\n";
      }
    } else if (AnalysisName == "uninitialized") {
      auto Res = mono::runIntraMonoUninitVariables(&F);
      if (Res) {
        for (auto *I : OrderedInsts) {
          const auto &InSet = Res->IN(I);
          OS << "  " << ValueToId.at(I) << " IN: ";
          formatValueSet(OS, InSet, ValueToId);
          OS << "\n";
        }
      }
    }
  }
}

void runIFDSAnalysis(Module &M, const std::string &AnalysisName,
                     OutputManager &OutMgr, const std::string &EngineName) {
  raw_ostream &OS = OutMgr.getStream(EngineName);
  bool firstFunc = true;

  if (AnalysisName == "reaching_defs") {
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;

      std::unordered_map<const llvm::Value *, std::string> ValueToId;
      std::vector<llvm::Instruction *> OrderedInsts;
      buildValueIds(&F, ValueToId, OrderedInsts);

      if (OutDir.empty() && firstFunc) {
        OS << "[" << EngineName << ":" << AnalysisName << "]\n";
        firstFunc = false;
      }

      OS << "FUNC " << F.getName().str() << "\n";

      ifds::ReachingDefinitionsAnalysis problem;
      ifds::IFDSSolver<ifds::ReachingDefinitionsAnalysis> solver(problem);
      solver.solve(M);

      auto allResults = solver.get_all_results();
      for (auto *I : OrderedInsts) {
        const llvm::Instruction *nextInst = I->getNextNode();
        if (!nextInst) {
          for (auto *Succ : successors(I->getParent())) {
            if (Succ->isLandingPad() || Succ->empty())
              continue;
            nextInst = &Succ->front();
            break;
          }
        }
        if (nextInst) {
          auto node = ifds::ExplodedSupergraph<ifds::DefinitionFact>::Node(
              nextInst, ifds::DefinitionFact::zero());
          auto It = allResults.find(node);
          OS << "  " << ValueToId.at(I) << " IN: ";
          if (It != allResults.end()) {
            formatIFDSFactSet(OS, It->second, ValueToId);
          }
          OS << "\n";
        }
      }
    }
  } else if (AnalysisName == "uninitialized") {
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;

      std::unordered_map<const llvm::Value *, std::string> ValueToId;
      std::vector<llvm::Instruction *> OrderedInsts;
      buildValueIds(&F, ValueToId, OrderedInsts);

      if (OutDir.empty() && firstFunc) {
        OS << "[" << EngineName << ":" << AnalysisName << "]\n";
        firstFunc = false;
      }

      OS << "FUNC " << F.getName().str() << "\n";

      ifds::UninitializedVariablesAnalysis problem;
      ifds::IFDSSolver<ifds::UninitializedVariablesAnalysis> solver(problem);
      solver.solve(M);

      auto allResults = solver.get_all_results();
      for (auto *I : OrderedInsts) {
        const llvm::Instruction *nextInst = I->getNextNode();
        if (!nextInst) {
          for (auto *Succ : successors(I->getParent())) {
            if (Succ->isLandingPad() || Succ->empty())
              continue;
            nextInst = &Succ->front();
            break;
          }
        }
        if (nextInst) {
          auto node = ifds::ExplodedSupergraph<ifds::UninitVarFact>::Node(
              nextInst, ifds::UninitVarFact::zero());
          auto It = allResults.find(node);
          OS << "  " << ValueToId.at(I) << " IN: ";
          if (It != allResults.end()) {
            formatIFDSFactSet(OS, It->second, ValueToId);
          }
          OS << "\n";
        }
      }
    }
  } else {
    if (OutDir.empty() && firstFunc) {
      OS << "[" << EngineName << ":" << AnalysisName << "]\n";
      OS << "  (not implemented for IFDS)\n";
    }
  }
}

bool isValidAnalysis(const std::string &Analysis) {
  static const std::set<std::string> valid = {
      "liveness",      "reaching_defs",   "uninitialized",
      "constant_prop", "available_exprs", "reachable"};
  return valid.count(Analysis) > 0;
}

bool isValidEngine(const std::string &Engine) {
  static const std::set<std::string> valid = {"elim", "mono", "ifds", "all"};
  return valid.count(Engine) > 0;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Dataflow engine diff testing\n");

  if (!isValidAnalysis(AnalysisOpt)) {
    errs() << "error: --analysis=" << AnalysisOpt << " not supported\n";
    errs() << "Available: liveness, reaching_defs, uninitialized, "
              "constant_prop, available_exprs, reachable\n";
    return 1;
  }

  if (!isValidEngine(EngineOpt)) {
    errs() << "error: --engine=" << EngineOpt << " not supported\n";
    errs() << "Available: elim, mono, ifds, all\n";
    return 1;
  }

  bool runElim = (EngineOpt == "elim" || EngineOpt == "all");
  bool runMono = (EngineOpt == "mono" || EngineOpt == "all");
  bool runIFDS = (EngineOpt == "ifds" || EngineOpt == "all");

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

  OutputManager OutMgr;

  if (runElim) {
    runEliminationAnalysis(*M, AnalysisOpt, OutMgr, "elim", nullptr);
  }

  if (runMono) {
    runMonoAnalysis(*M, AnalysisOpt, OutMgr, "mono");
  }

  if (runIFDS) {
    runIFDSAnalysis(*M, AnalysisOpt, OutMgr, "ifds");
  }

  OutMgr.close();

  return 0;
}
