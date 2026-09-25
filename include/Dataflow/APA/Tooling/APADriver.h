#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Solver/Equations/Options.h"
#include "Dataflow/Tooling/ToolSupport.h"

#include <string>

namespace elimination::tooling {

struct APADriverOptions {
  std::string OutDir = "";
  bool Stdout = false;
  std::string Analysis = "reachable";
  std::string EntryFunction = "main";
  std::string ElimMethod = "state";
  bool DumpProfile = false;
  bool ProfileOnly = false;
  bool DumpExprs = false;
  unsigned AffineMaxTracked = 32;

  std::string Ordering = "default";
  double OrderStructCap = 64.0;
  double OrderExprCap = 4096.0;
  double OrderStarCap = 256.0;
  unsigned OrderSizeCap = 1024;
  bool OrderFullRescore = false;
  bool OrderTrace = false;
  bool OrderSparse = false;
  unsigned long long OrderSeed = 0;
  std::string OrderExplicit = "";

  bool Ean = false;
  bool Greedy = false;
  bool EanMonotone = false;
  std::string EanLaws = "safe";
  std::string EanCost = "uniform";
  unsigned EanRoundLimit = 0;
  unsigned EanNodeLimit = 0;
  double EanTimeLimit = 0.0;
  bool MeasurePeak = false;
  unsigned MaxFuncInsts = 0;
  unsigned EanMinNodes = 0;
  unsigned InterpRepeat = 1;
  std::string InterEngine = "context";
  bool MemoInterp = false;
  std::string Interp = "generic";
};

struct APADriverState {
  bool HadSolveError = false;
};

elimination::OrderingPolicy parseOrderingPolicy(llvm::StringRef OrderingOpt);
elimination::OrderPolicyOptions buildOrderOpts(const APADriverOptions &Opts);
elimination::EliminationOptions buildElimOpts(const APADriverOptions &Opts);
elimination::PathSummaryEquationOptions buildInterSummaryOpts(const APADriverOptions &Opts);
void validateDriverOptions(APADriverOptions &Opts);

struct AnalysisHandler final {
  llvm::StringRef Name;
  bool ModuleScoped = false;
  void (*RunFunction)(llvm::raw_ostream &, const lotus::dataflow_tool::FunctionView &,
                      const elimination::EliminationOptions &,
                      const APADriverOptions &, APADriverState &) = nullptr;
  void (*RunModule)(llvm::raw_ostream &, llvm::Module &, llvm::Function &,
                    const APADriverOptions &, APADriverState &) = nullptr;
};

llvm::ArrayRef<AnalysisHandler> getAvailableAnalyses();
const AnalysisHandler *findAnalysisHandler(llvm::StringRef Name);

int runApaDriver(llvm::Module &M, llvm::raw_ostream &OS, const APADriverOptions &Opts);

}

