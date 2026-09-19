/*
 * lotus-dfa-npa
 *
 * Dataflow testing tool: NPA engine.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/NPA/Analyses/Inter/ConstantPropagation.h"
#include "Dataflow/NPA/Analyses/Inter/Interval.h"
#include "Dataflow/NPA/Analyses/Inter/LiveVariables.h"
#include "Dataflow/NPA/Analyses/Inter/MaybeUninitialized.h"
#include "Dataflow/NPA/Analyses/Inter/Nullability.h"
#include "Dataflow/NPA/Analyses/Inter/ReachingDefinitions.h"
#include "Dataflow/NPA/Analyses/Intra/LiveVariables.h"
#include "Dataflow/NPA/Analyses/Intra/ReachableBlocks.h"
#include "Dataflow/NPA/Analyses/Intra/ReachingDefinitions.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"
#include "Dataflow/NPA/LLVM/BitVectorSolver.h"
#include "Dataflow/Tooling/ToolSupport.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                          cl::Required);
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<bool> StdoutOpt(
    "stdout",
    cl::desc("Write analysis results to stdout when --out-dir is not set"),
    cl::init(false));
static cl::opt<bool> PrintBlockResultsOpt(
    "print-block-results",
    cl::desc("Print per-block facts in addition to the analysis profile"),
    cl::init(false));
static cl::opt<std::string>
    AnalysisOpt("analysis",
                cl::desc("Analysis: liveness (default), reaching_defs, "
                         "reachable, inter_liveness, inter_reaching_defs, "
                         "inter_uninitialized, constant_prop, interval, "
                         "nullability"),
                cl::init("liveness"));
static cl::opt<std::string>
    SolverOpt("solver", cl::desc("Solver: newton (default), kleene"),
              cl::init("newton"));
static cl::opt<std::string> LinearSolverOpt(
    "linear-solver",
    cl::desc("Newton linear solver: scc (default), adaptive_scc, tensor"),
    cl::init("scc"));
static cl::opt<std::string> NewtonRoundOpt(
    "newton-round",
    cl::desc("Newton round construction: dense (default), static, "
             "always_maybe, sparse"),
    cl::init("dense"));

namespace {

using lotus::dataflow_tool::FunctionView;
using ModuleValueIdMap = std::unordered_map<const Value *, std::string>;

struct BlockView final {
  Function &Func;
  FunctionView InstView;
  std::vector<const BasicBlock *> OrderedBlocks;
  std::unordered_map<const BasicBlock *, std::string> BlockToId;
  std::vector<std::string> BitLabels;
};

struct ModuleView final {
  ModuleValueIdMap ValueToId;
  std::vector<std::string> BitLabels;
  std::vector<Function *> OrderedFunctions;
};

BlockView buildBlockView(Function &F) {
  BlockView View{F, lotus::dataflow_tool::buildFunctionView(F), {}, {}, {}};

  unsigned BlockIdx = 0;
  for (auto &BB : F) {
    View.OrderedBlocks.push_back(&BB);
    View.BlockToId[&BB] = "bb" + std::to_string(BlockIdx++);
  }

  for (auto &Arg : F.args())
    View.BitLabels.push_back(View.InstView.ValueToId.at(&Arg));

  for (auto *I : View.InstView.OrderedInsts) {
    if (!I->getType()->isVoidTy())
      View.BitLabels.push_back(View.InstView.ValueToId.at(I));
  }

  if (View.BitLabels.empty())
    View.BitLabels.push_back("bit0");

  return View;
}

ModuleView buildModuleView(Module &M) {
  ModuleView View;

  unsigned ArgIdx = 0;
  unsigned InstIdx = 0;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    View.OrderedFunctions.push_back(&F);
    for (auto &Arg : F.args()) {
      std::string Id = "arg" + std::to_string(ArgIdx++);
      View.ValueToId.emplace(&Arg, Id);
      View.BitLabels.push_back(Id);
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.getType()->isVoidTy())
          continue;
        std::string Id = "i" + std::to_string(InstIdx++);
        View.ValueToId.emplace(&I, Id);
        View.BitLabels.push_back(Id);
      }
    }
  }

  if (View.BitLabels.empty())
    View.BitLabels.push_back("bit0");

  return View;
}

std::string lookupValueId(const Value *V, const ModuleValueIdMap &ValueToId,
                          StringRef Fallback = "v") {
  auto It = ValueToId.find(V);
  if (It != ValueToId.end())
    return It->second;
  return std::string(Fallback);
}

std::string formatAPInt(const APInt &Value, bool Signed = true) {
  std::string Buffer;
  raw_string_ostream OS(Buffer);
  Value.print(OS, Signed);
  return OS.str();
}

std::string
formatConstantPropagationValue(const npa::ConstantPropagationValue &Value) {
  if (!Value.isConstant())
    return "top";
  return "const(" + formatAPInt(Value.constant) + ")";
}

std::string formatInterval(const npa::Interval &Interval) {
  if (Interval.bottom)
    return "bottom";
  std::string Lower = Interval.hasLower ? formatAPInt(Interval.lower) : "-inf";
  std::string Upper = Interval.hasUpper ? formatAPInt(Interval.upper) : "+inf";
  return (Twine(Interval.ordering == npa::IntervalOrdering::Unsigned ? "u"
                                                                     : "s") +
          "[" + Lower + "," + Upper + "]")
      .str();
}

std::vector<std::string>
buildMaybeUninitializedLabels(Module &M, const ModuleValueIdMap &ValueToId) {
  std::vector<std::string> Labels;
  std::unordered_map<const Value *, unsigned> ValueBits;
  std::unordered_map<const Value *, unsigned> MemoryBits;
  auto addValueBit = [&](const Value *V) {
    if (!V || ValueBits.count(V))
      return;
    ValueBits.emplace(V, static_cast<unsigned>(Labels.size()));
    Labels.push_back(lookupValueId(V, ValueToId));
  };
  auto addMemoryBit = [&](const Value *V) {
    if (!V || !V->getType()->isPointerTy())
      return;
    const Value *Memory = getUnderlyingObject(V->stripPointerCasts());
    if (!Memory || MemoryBits.count(Memory))
      return;
    MemoryBits.emplace(Memory, static_cast<unsigned>(Labels.size()));
    Labels.push_back("mem(" + lookupValueId(Memory, ValueToId) + ")");
  };

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    for (auto &Arg : F.args()) {
      addValueBit(&Arg);
      addMemoryBit(&Arg);
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!I.getType()->isVoidTy())
          addValueBit(&I);
        if (isa<AllocaInst>(&I))
          addMemoryBit(&I);
        if (auto *Load = dyn_cast<LoadInst>(&I))
          addMemoryBit(Load->getPointerOperand());
        if (auto *Store = dyn_cast<StoreInst>(&I))
          addMemoryBit(Store->getPointerOperand());
        if (auto *Call = dyn_cast<CallBase>(&I)) {
          for (Use &Arg : Call->args())
            addMemoryBit(Arg.get());
        }
      }
    }
  }

  if (Labels.empty())
    Labels.push_back("bit0");
  return Labels;
}

template <typename BitMapT>
void assignBitLabels(std::vector<std::string> &Labels, const BitMapT &Bits,
                     const ModuleValueIdMap &ValueToId, StringRef Prefix) {
  for (const auto &Entry : Bits) {
    if (Entry.second >= Labels.size())
      Labels.resize(Entry.second + 1);
    std::string Label =
        Prefix.empty()
            ? lookupValueId(Entry.first, ValueToId)
            : (Prefix + "(" + lookupValueId(Entry.first, ValueToId) + ")")
                  .str();
    if (Labels[Entry.second].empty() || Label < Labels[Entry.second])
      Labels[Entry.second] = std::move(Label);
  }
}

template <typename BitMapT>
void assignBitVectorLabels(std::vector<std::string> &Labels,
                           const BitMapT &Bits,
                           const ModuleValueIdMap &ValueToId,
                           StringRef Prefix) {
  for (const auto &Entry : Bits) {
    for (unsigned Bit : Entry.second) {
      if (Bit >= Labels.size())
        Labels.resize(Bit + 1);
      std::string Label =
          Prefix.empty()
              ? lookupValueId(Entry.first, ValueToId)
              : (Prefix + "(" + lookupValueId(Entry.first, ValueToId) + ")")
                    .str();
      if (Labels[Bit].empty() || Label < Labels[Bit])
        Labels[Bit] = std::move(Label);
    }
  }
}

void finalizeBitLabels(std::vector<std::string> &Labels) {
  if (Labels.empty()) {
    Labels.push_back("bit0");
    return;
  }
  for (size_t I = 0; I < Labels.size(); ++I) {
    if (Labels[I].empty())
      Labels[I] = "bit" + std::to_string(I);
  }
}

npa::SolverStrategy parseSolverStrategy(StringRef Name) {
  if (Name == "kleene")
    return npa::SolverStrategy::Kleene;
  return npa::SolverStrategy::Newton;
}

npa::LinearStrategy parseLinearStrategy(StringRef Name) {
  if (Name == "adaptive_scc")
    return npa::LinearStrategy::AdaptiveScc;
  if (Name == "tensor")
    return npa::LinearStrategy::TensorProduct;
  return npa::LinearStrategy::SCC;
}

npa::NewtonRoundStrategy parseNewtonRoundStrategy(StringRef Name) {
  if (Name == "static")
    return npa::NewtonRoundStrategy::Static;
  if (Name == "always_maybe")
    return npa::NewtonRoundStrategy::AlwaysMaybe;
  if (Name == "sparse")
    return npa::NewtonRoundStrategy::Sparse;
  return npa::NewtonRoundStrategy::Dense;
}

void printNewtonProfile(raw_ostream &OS, const npa::Stat &Stats) {
  if (NewtonRoundOpt.getNumOccurrences() == 0 || Stats.newton_rounds.empty())
    return;
  OS << "  [profile] equation_validation_seconds="
     << Stats.equation_validation_time
     << " newton_initialization_seconds="
     << Stats.newton_initialization_time << "\n";
  OS << "  [profile] sparse_index_seconds=" << Stats.occurrence_index_time
     << " indexed_occurrences=" << Stats.indexed_derivative_occurrences
     << " queried_occurrences=" << Stats.queried_derivative_occurrences
     << " retained_occurrences=" << Stats.retained_derivative_occurrences
     << " materialized_terms=" << Stats.materialized_derivative_terms << "\n";
  OS << "  [profile] round_discovery_seconds=" << Stats.round_discovery_time
     << " round_materialization_seconds=" << Stats.round_materialization_time
     << " round_setup_seconds="
     << Stats.round_discovery_time + Stats.round_materialization_time
     << " linear_solve_seconds=" << Stats.linear_solve_time << "\n";
  for (size_t I = 0; I < Stats.newton_rounds.size(); ++I) {
    const auto &Round = Stats.newton_rounds[I];
    OS << "  [profile] newton_round=" << I
       << " active_coordinates=" << Round.active_coordinates
       << " queried_occurrences=" << Round.queried_occurrences
       << " retained_occurrences=" << Round.retained_occurrences
       << " materialized_terms=" << Round.materialized_derivative_terms
       << " discovery_seconds=" << Round.discovery_time
       << " materialization_seconds=" << Round.materialization_time
       << " setup_seconds=" << Round.discovery_time + Round.materialization_time
       << " linear_seconds=" << Round.linear_solve_time << "\n";
  }
}

void formatBitSet(raw_ostream &OS, const APInt &Bits,
                  const std::vector<std::string> &BitLabels) {
  bool First = true;
  const unsigned Width =
      std::min<unsigned>(Bits.getBitWidth(), BitLabels.size());
  for (unsigned Bit = 0; Bit < Width; ++Bit) {
    if (!Bits[Bit])
      continue;
    if (!First)
      OS << ",";
    OS << BitLabels[Bit];
    First = false;
  }
}

void formatBitSet(raw_ostream &OS,
                  const npa::GenKillTransformer::fact_type &Bits,
                  const std::vector<std::string> &BitLabels) {
  bool First = true;
  for (unsigned Bit : Bits) {
    if (Bit >= BitLabels.size())
      continue;
    if (!First)
      OS << ",";
    OS << BitLabels[Bit];
    First = false;
  }
}

template <typename Printer>
void printBlockStates(raw_ostream &OS, const BlockView &View,
                      Printer &&PrintState) {
  if (!PrintBlockResultsOpt)
    return;
  for (const BasicBlock *BB : View.OrderedBlocks) {
    OS << "  " << View.BlockToId.at(BB) << " IN: ";
    PrintState(BB);
    OS << "\n";
  }
}

void runLiveness(raw_ostream &OS, Function &F, npa::SolverStrategy Strategy,
                 npa::LinearStrategy LinearStrategy,
                 npa::NewtonRoundStrategy RoundStrategy) {
  BlockView View = buildBlockView(F);
  auto Result =
      npa::LiveVariables::run(F, Strategy, LinearStrategy, RoundStrategy);
  lotus::dataflow_tool::emitFunctionHeader(OS, F);
  printNewtonProfile(OS, Result.stats);
  printBlockStates(OS, View, [&](const BasicBlock *BB) {
    auto It = Result.IN.find(BB);
    if (It != Result.IN.end())
      formatBitSet(OS, It->second, View.BitLabels);
  });
}

void runReachingDefinitions(raw_ostream &OS, Function &F,
                            npa::SolverStrategy Strategy,
                            npa::LinearStrategy LinearStrategy,
                            npa::NewtonRoundStrategy RoundStrategy) {
  BlockView View = buildBlockView(F);
  auto Result =
      npa::ReachingDefinitions::run(F, Strategy, LinearStrategy, RoundStrategy);
  lotus::dataflow_tool::emitFunctionHeader(OS, F);
  printNewtonProfile(OS, Result.stats);
  printBlockStates(OS, View, [&](const BasicBlock *BB) {
    auto It = Result.IN.find(BB);
    if (It != Result.IN.end())
      formatBitSet(OS, It->second, View.BitLabels);
  });
}

void runReachable(raw_ostream &OS, Function &F, npa::SolverStrategy Strategy,
                  npa::LinearStrategy LinearStrategy,
                  npa::NewtonRoundStrategy RoundStrategy) {
  BlockView View = buildBlockView(F);
  auto Reachable =
      npa::ReachableBlocks::run(F, Strategy, LinearStrategy, RoundStrategy);
  lotus::dataflow_tool::emitFunctionHeader(OS, F);
  printBlockStates(OS, View, [&](const BasicBlock *BB) {
    OS << (Reachable.count(BB) ? "reachable" : "unreachable");
  });
}

template <typename Printer>
void printModuleBlockStates(raw_ostream &OS, Module &M, Printer &&PrintState) {
  if (!PrintBlockResultsOpt)
    return;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    BlockView View = buildBlockView(F);
    lotus::dataflow_tool::emitFunctionHeader(OS, F);
    for (const BasicBlock *BB : View.OrderedBlocks) {
      OS << "  " << View.BlockToId.at(BB) << " IN: ";
      PrintState(BB);
      OS << "\n";
    }
  }
}

void runInterproceduralLiveness(raw_ostream &OS, Module &M,
                                npa::LinearStrategy LinearStrategy,
                                npa::NewtonRoundStrategy RoundStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterLiveVariables::run(
      M, false, LinearStrategy,
      npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      RoundStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  printNewtonProfile(OS, Result.status.summary_solve);
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It != Result.blockFacts.end())
      formatBitSet(OS, It->second, View.BitLabels);
  });
}

void runInterproceduralReachingDefinitions(
    raw_ostream &OS, Module &M, npa::LinearStrategy LinearStrategy,
    npa::NewtonRoundStrategy RoundStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterReachingDefinitions::run(
      M, false, LinearStrategy,
      npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      RoundStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  printNewtonProfile(OS, Result.status.summary_solve);
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It != Result.blockFacts.end())
      formatBitSet(OS, It->second, View.BitLabels);
  });
}

void runInterproceduralMaybeUninitialized(
    raw_ostream &OS, Module &M, npa::LinearStrategy LinearStrategy,
    npa::NewtonRoundStrategy RoundStrategy) {
  const ModuleView View = buildModuleView(M);
  const auto Labels = buildMaybeUninitializedLabels(M, View.ValueToId);
  auto Result = npa::InterMaybeUninitialized::run(
      M, false, LinearStrategy,
      npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      RoundStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  printNewtonProfile(OS, Result.status.summary_solve);
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It != Result.blockFacts.end())
      formatBitSet(OS, It->second, Labels);
  });
}

void runInterproceduralConstantPropagation(
    raw_ostream &OS, Module &M, npa::LinearStrategy LinearStrategy,
    npa::NewtonRoundStrategy RoundStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterConstantPropagation::run(
      M, false, LinearStrategy,
      npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      RoundStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  printNewtonProfile(OS, Result.status.summary_solve);
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It == Result.blockFacts.end())
      return;
    OS << (It->second.reachable ? "reachable:" : "unreachable:");
    lotus::dataflow_tool::formatValueMap(
        OS, It->second.values, View.ValueToId,
        [](const npa::ConstantPropagationValue &Value) {
          return formatConstantPropagationValue(Value);
        });
  });
}

void runInterproceduralInterval(raw_ostream &OS, Module &M,
                                npa::LinearStrategy LinearStrategy,
                                npa::NewtonRoundStrategy RoundStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterIntervalAnalysis::run(
      M, false, LinearStrategy,
      npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      RoundStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  printNewtonProfile(OS, Result.status.summary_solve);
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It == Result.blockFacts.end())
      return;
    OS << (It->second.reachable ? "reachable:" : "unreachable:");
    lotus::dataflow_tool::formatValueMap(
        OS, It->second.values, View.ValueToId,
        [](const npa::Interval &Interval) { return formatInterval(Interval); });
  });
}

void runInterproceduralNullability(raw_ostream &OS, Module &M,
                                   npa::LinearStrategy LinearStrategy,
                                   npa::NewtonRoundStrategy RoundStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterNullability::run(
      M, false, LinearStrategy,
      npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      RoundStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  printNewtonProfile(OS, Result.status.summary_solve);
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  std::vector<std::string> Labels;
  assignBitLabels(Labels, Result.valueBits, View.ValueToId, "");
  assignBitLabels(Labels, Result.memoryBits, View.ValueToId, "mem");
  assignBitVectorLabels(Labels, Result.pointerMemoryBits, View.ValueToId,
                        "mem");
  finalizeBitLabels(Labels);
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It != Result.blockFacts.end())
      formatBitSet(OS, It->second, Labels);
  });
}

struct AnalysisHandler final {
  StringRef Name;
  bool ModuleScoped = false;
  void (*RunFunction)(raw_ostream &, Function &, npa::SolverStrategy,
                      npa::LinearStrategy, npa::NewtonRoundStrategy) = nullptr;
  void (*RunModule)(raw_ostream &, Module &, npa::LinearStrategy,
                    npa::NewtonRoundStrategy) = nullptr;
};

const AnalysisHandler Handlers[] = {
    {"liveness", false, &runLiveness, nullptr},
    {"reaching_defs", false, &runReachingDefinitions, nullptr},
    {"reachable", false, &runReachable, nullptr},
    {"inter_liveness", true, nullptr, &runInterproceduralLiveness},
    {"inter_reaching_defs", true, nullptr,
     &runInterproceduralReachingDefinitions},
    {"inter_uninitialized", true, nullptr,
     &runInterproceduralMaybeUninitialized},
    {"inter_constant_prop", true, nullptr,
     &runInterproceduralConstantPropagation},
    {"inter_interval", true, nullptr, &runInterproceduralInterval},
    {"inter_nullability", true, nullptr, &runInterproceduralNullability},
};

void runIntraproceduralAnalysesOnModule(
    raw_ostream &OS, Module &M, const AnalysisHandler &Handler,
    npa::SolverStrategy Strategy, npa::LinearStrategy LinearStrategy,
    npa::NewtonRoundStrategy RoundStrategy) {
  assert(!Handler.ModuleScoped &&
         "module-scoped interprocedural analyses schedule inside the engine");
  for (auto &F : M) {
    if (!F.isDeclaration())
      Handler.RunFunction(OS, F, Strategy, LinearStrategy, RoundStrategy);
  }
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "NPA engine testing\n");

  if (SolverOpt != "newton" && SolverOpt != "kleene") {
    errs() << "error: unknown NPA solver '" << SolverOpt << "'\n";
    return 1;
  }
  if (LinearSolverOpt != "scc" && LinearSolverOpt != "adaptive_scc" &&
      LinearSolverOpt != "tensor") {
    errs() << "error: unknown NPA linear solver '" << LinearSolverOpt << "'\n";
    return 1;
  }
  if (NewtonRoundOpt != "dense" && NewtonRoundOpt != "static" &&
      NewtonRoundOpt != "always_maybe" && NewtonRoundOpt != "sparse") {
    errs() << "error: unknown Newton round strategy '" << NewtonRoundOpt
           << "'\n";
    return 1;
  }
  if (SolverOpt == "kleene" && NewtonRoundOpt != "dense") {
    errs() << "error: --newton-round applies only to --solver=newton\n";
    return 1;
  }

  LLVMContext Context;
  SMDiagnostic Err;
  auto M = lotus::dataflow_tool::loadModuleOrReport(InputFilename, Context, Err,
                                                    argv[0]);
  if (!M)
    return 1;

  lotus::dataflow_tool::prepareModule(*M);

  raw_null_ostream NullOS;
  std::unique_ptr<raw_fd_ostream> FileOS;
  std::error_code EC;
  raw_ostream &OS = lotus::dataflow_tool::selectOutputStream(
      StdoutOpt, OutDir, "npa.txt", FileOS, NullOS, EC);
  if (EC) {
    errs() << "error: cannot create " << OutDir << "/npa.txt: " << EC.message()
           << "\n";
    return 1;
  }

  const auto *Handler =
      lotus::dataflow_tool::findHandler(AnalysisOpt, Handlers);
  if (!Handler) {
    errs() << "error: unknown NPA analysis '" << AnalysisOpt << "'\n";
    return 1;
  }

  if (Handler->ModuleScoped && SolverOpt != "newton") {
    errs() << "error: NPA analysis '" << AnalysisOpt
           << "' uses the module-level interprocedural engine and does not "
              "support --solver="
           << SolverOpt << "\n";
    return 1;
  }

  const npa::SolverStrategy Strategy = parseSolverStrategy(SolverOpt);
  const npa::LinearStrategy LinearStrategy =
      parseLinearStrategy(LinearSolverOpt);
  const npa::NewtonRoundStrategy RoundStrategy =
      parseNewtonRoundStrategy(NewtonRoundOpt);
  OS << "[npa:" << AnalysisOpt;
  if (Handler->ModuleScoped)
    OS << ":module";
  else
    OS << ":" << SolverOpt;
  OS << ":linear=" << LinearSolverOpt;
  if (NewtonRoundOpt.getNumOccurrences() != 0)
    OS << ":round=" << NewtonRoundOpt;
  OS << "]\n";
  if (Handler->ModuleScoped)
    Handler->RunModule(OS, *M, LinearStrategy, RoundStrategy);
  else
    runIntraproceduralAnalysesOnModule(OS, *M, *Handler, Strategy,
                                       LinearStrategy, RoundStrategy);

  return 0;
}
