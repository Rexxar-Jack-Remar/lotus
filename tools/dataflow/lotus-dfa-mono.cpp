/*
 * lotus-dfa-mono
 *
 * Dataflow testing tool: Mono engine.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/Mono/Analyses/Intra/IntraConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/Mono/Analyses/Intra/IntraReachable.h"
#include "Dataflow/Mono/Analyses/Intra/IntraUninitVariables.h"
#include "ToolSupport.h"

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
static cl::opt<std::string>
    AnalysisOpt("analysis",
                cl::desc("Analysis: liveness (default), reachable, "
                         "constant_prop, uninitialized"),
                cl::init("liveness"));

namespace {

using lotus::dataflow_tool::FunctionView;
using lotus::dataflow_tool::ValueIdMap;

template <typename T>
void formatValueSet(raw_ostream &OS, const std::set<T> &S,
                    const ValueIdMap &ValueToId) {
  std::vector<std::string> ids;
  for (const Value *V : S) {
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
void formatConstPropMap(raw_ostream &OS,
                        const std::unordered_map<const Value *, ValueType> &M,
                        const ValueIdMap &ValueToId) {
  std::vector<std::string> entries;
  for (const auto &Entry : M) {
    std::ostringstream ss;
    auto It = ValueToId.find(Entry.first);
    ss << (It != ValueToId.end() ? It->second : "v") << "="
       << formatMonoConstantValue(Entry.second);
    entries.push_back(ss.str());
  }
  std::sort(entries.begin(), entries.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i)
      OS << ",";
    OS << entries[i];
  }
}

template <typename Printer>
void printInstructionStates(raw_ostream &OS, const FunctionView &View,
                            Printer &&PrintState) {
  for (auto *I : View.OrderedInsts) {
    OS << "  " << View.ValueToId.at(I) << " IN: ";
    PrintState(I);
    OS << "\n";
  }
}

void runLiveness(raw_ostream &OS, const FunctionView &View) {
  if (auto Res = mono::runLiveVariablesAnalysis(&View.Function))
    printInstructionStates(OS, View, [&](Instruction *I) {
      formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

void runReachable(raw_ostream &OS, const FunctionView &View) {
  if (auto Res = mono::runReachableAnalysis(&View.Function))
    printInstructionStates(OS, View, [&](Instruction *I) {
      formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

void runConstantPropagation(raw_ostream &OS, const FunctionView &View) {
  auto Res = mono::runIntraMonoConstantPropagation(&View.Function);
  printInstructionStates(OS, View, [&](Instruction *I) {
    auto It = Res.find(I);
    if (It != Res.end())
      formatConstPropMap(OS, It->second, View.ValueToId);
  });
}

void runUninitialized(raw_ostream &OS, const FunctionView &View) {
  if (auto Res = mono::runIntraMonoUninitVariables(&View.Function))
    printInstructionStates(OS, View, [&](Instruction *I) {
      formatValueSet(OS, Res->IN(I), View.ValueToId);
    });
}

struct AnalysisHandler final {
  StringRef Name;
  void (*Run)(raw_ostream &, const FunctionView &);
};

const AnalysisHandler *findHandler(StringRef Name) {
  static const AnalysisHandler Handlers[] = {
      {"liveness", &runLiveness},
      {"reachable", &runReachable},
      {"constant_prop", &runConstantPropagation},
      {"uninitialized", &runUninitialized},
  };
  for (const auto &Handler : Handlers)
    if (Handler.Name == Name)
      return &Handler;
  return nullptr;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Mono engine testing\n");

  LLVMContext Context;
  SMDiagnostic Err;
  auto M = lotus::dataflow_tool::loadModuleOrReport(InputFilename, Context, Err,
                                                    argv[0]);
  if (!M)
    return 1;

  lotus::dataflow_tool::prepareModule(*M);

  raw_ostream *OutOS = &outs();
  std::unique_ptr<raw_fd_ostream> FileOS;
  if (!OutDir.empty()) {
    std::error_code EC;
    FileOS =
        lotus::dataflow_tool::openOutputFileOrReport(OutDir, "mono.txt", EC);
    if (EC) {
      errs() << "error: cannot create " << OutDir
             << "/mono.txt: " << EC.message() << "\n";
      return 1;
    }
    OutOS = FileOS.get();
  }
  raw_ostream &OS = *OutOS;

  const auto *Handler = findHandler(AnalysisOpt);
  if (!Handler) {
    errs() << "error: unknown mono analysis '" << AnalysisOpt << "'\n";
    return 1;
  }

  OS << "[mono:" << AnalysisOpt << "]\n";
  for (auto &F : *M) {
    if (F.isDeclaration())
      continue;

    auto View = lotus::dataflow_tool::buildFunctionView(F);
    OS << "FUNC " << F.getName() << "\n";
    Handler->Run(OS, View);
  }

  return 0;
}
