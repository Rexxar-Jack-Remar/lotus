#include "Dataflow/APA/Tooling/APADriver.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Analyses/Inter/AffineEqualities.h"
#include "Dataflow/APA/Analyses/Inter/AvailableExpressions.h"
#include "Dataflow/APA/Analyses/Inter/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/Inter/Lockset.h"
#include "Dataflow/APA/Analyses/Inter/NonNull.h"
#include "Dataflow/APA/Analyses/Inter/Reachability.h"
#include "Dataflow/APA/Analyses/Inter/ReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Inter/Sign.h"
#include "Dataflow/APA/Analyses/Inter/UninitializedVariables.h"
#include "Dataflow/APA/Analyses/Intra/AffineEqualities.h"
#include "Dataflow/APA/Analyses/Intra/AvailableExpressions.h"
#include "Dataflow/APA/Analyses/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/Intra/Lockset.h"
#include "Dataflow/APA/Analyses/Intra/NonNull.h"
#include "Dataflow/APA/Analyses/Intra/Reachability.h"
#include "Dataflow/APA/Analyses/Intra/ReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Intra/Sign.h"
#include "Dataflow/APA/Analyses/Intra/UninitializedVariables.h"
#include "Dataflow/APA/Tooling/APADiagnostics.h"
#include "Dataflow/APA/Tooling/APAFormatting.h"
#include "Dataflow/Tooling/ToolSupport.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace llvm;

namespace elimination::tooling {

using lotus::dataflow_tool::FunctionView;
using lotus::dataflow_tool::ValueIdMap;

elimination::OrderingPolicy parseOrderingPolicy(StringRef OrderingOpt) {
  using P = elimination::OrderingPolicy;
  const std::pair<const char *, P> Policies[] = {
      {"default", P::Default},
      {"cost-aware", P::CostAware},
      {"structural", P::Structural},
      {"expr-aware", P::ExpressionAware},
      {"expression-aware", P::ExpressionAware},
      {"star-risk", P::StarRisk},
      {"hybrid", P::Hybrid},
      {"rpo", P::ReversePostOrder},
      {"random", P::Random},
      {"min-degree", P::MinDegree},
      {"min-fill", P::MinFill},
      {"explicit", P::Explicit}};
  for (const auto &Entry : Policies) {
    if (OrderingOpt == Entry.first) {
      return Entry.second;
    }
  }
  throw std::invalid_argument("unknown APA ordering policy: " + OrderingOpt.str());
}

elimination::OrderPolicyOptions buildOrderOpts(const APADriverOptions &Opts) {
  elimination::OrderPolicyOptions PolicyOpts;
  PolicyOpts.StructuralCap = Opts.OrderStructCap;
  PolicyOpts.ExpressionCap = Opts.OrderExprCap;
  PolicyOpts.StarCap = Opts.OrderStarCap;
  PolicyOpts.DAGSizeCap = Opts.OrderSizeCap;
  PolicyOpts.Incremental = !Opts.OrderFullRescore;
  PolicyOpts.RecordTrace = Opts.OrderTrace;
  PolicyOpts.MeasureLiveNodes = Opts.MeasurePeak;
  PolicyOpts.UseSparseElimination = Opts.OrderSparse;
  PolicyOpts.RandomSeed = Opts.OrderSeed;
  if (!Opts.OrderExplicit.empty()) {
    if (Opts.OrderExplicit.back() == ',') {
      throw std::invalid_argument(
          "APA explicit order must not end with a comma");
    }
    std::stringstream Input(Opts.OrderExplicit);
    std::string Token;
    while (std::getline(Input, Token, ',')) {
      unsigned long long Index = 0;
      if (Token.empty() || StringRef(Token).getAsInteger(10, Index)) {
        throw std::invalid_argument(
            "APA explicit order must contain unsigned indices");
      }
      PolicyOpts.ExplicitOrder.push_back(static_cast<std::size_t>(Index));
    }
  }
  return PolicyOpts;
}

elimination::EliminationOptions buildElimOpts(const APADriverOptions &Opts) {
  auto ElimOpts = lotus::dataflow_tool::parseEliminationOptions(Opts.ElimMethod);
  ElimOpts.Ordering = parseOrderingPolicy(Opts.Ordering);
  ElimOpts.Order = buildOrderOpts(Opts);
  ElimOpts.EnableEAN = Opts.Ean;
  ElimOpts.EnableGreedy = Opts.Greedy;
  ElimOpts.EANLaws = (Opts.EanLaws == "kleene")
                         ? elimination::ean::LawProfile::kleeneAlgebra()
                         : elimination::ean::LawProfile::safeMinimal();
  ElimOpts.EANCost = (Opts.EanCost == "dag") ? elimination::ean::CostModel::dag()
                                           : elimination::ean::CostModel::uniform();
  elimination::ean::Budget B = elimination::ean::Budget::unbounded();
  if (Opts.EanRoundLimit)
    B.roundLimit = Opts.EanRoundLimit;
  if (Opts.EanNodeLimit)
    B.nodeLimit = Opts.EanNodeLimit;
  if (Opts.EanTimeLimit > 0.0)
    B.timeLimitSec = Opts.EanTimeLimit;
  ElimOpts.EANBudget = B;
  ElimOpts.MeasurePeakNodes = Opts.MeasurePeak;
  ElimOpts.EANMinNodes = Opts.EanMinNodes;
  ElimOpts.EANMonotone = Opts.EanMonotone;
  ElimOpts.InterpRepeat = Opts.InterpRepeat ? Opts.InterpRepeat : 1;
  ElimOpts.InterpMemo = Opts.MemoInterp;
  return ElimOpts;
}

elimination::PathSummaryEquationOptions buildInterSummaryOpts(const APADriverOptions &Opts) {
  elimination::PathSummaryEquationOptions SummaryOpts;
  SummaryOpts.Ordering = parseOrderingPolicy(Opts.Ordering);
  SummaryOpts.Order = buildOrderOpts(Opts);
  auto &E = SummaryOpts.EAN;
  E.EnableEAN = Opts.Ean;
  E.EnableGreedy = Opts.Greedy;
  E.EANLaws = (Opts.EanLaws == "kleene")
                  ? elimination::ean::LawProfile::kleeneAlgebra()
                  : elimination::ean::LawProfile::safeMinimal();
  E.EANCost = (Opts.EanCost == "dag") ? elimination::ean::CostModel::dag()
                                    : elimination::ean::CostModel::uniform();
  elimination::ean::Budget B = elimination::ean::Budget::unbounded();
  if (Opts.EanRoundLimit)
    B.roundLimit = Opts.EanRoundLimit;
  if (Opts.EanNodeLimit)
    B.nodeLimit = Opts.EanNodeLimit;
  if (Opts.EanTimeLimit > 0.0)
    B.timeLimitSec = Opts.EanTimeLimit;
  E.EANBudget = B;
  E.EANMinNodes = Opts.EanMinNodes;
  E.EANMonotone = Opts.EanMonotone;
  E.InterpRepeat = Opts.InterpRepeat ? Opts.InterpRepeat : 1;
  return SummaryOpts;
}

void validateDriverOptions(APADriverOptions &Opts) {
  (void)parseOrderingPolicy(Opts.Ordering);
  elimination::order::validateOrderOptions(
      buildOrderOpts(Opts), elimination::OrderingPolicy::Default, 0);
  if (Opts.InterEngine != "context" && Opts.InterEngine != "expanded" &&
      Opts.InterEngine != "modular") {
    throw std::invalid_argument("unknown interprocedural engine: " +
                                Opts.InterEngine);
  }
  if (Opts.InterEngine == "modular" && Opts.Analysis != "inter_reachable") {
    throw std::invalid_argument(
        "the modular CLI currently supports inter_reachable only");
  }
  if (Opts.OrderTrace) {
    Opts.DumpProfile = true;
  }
}

namespace {

template <typename ResultT, typename Printer>
void dumpAnalysisResult(raw_ostream &OS, const FunctionView &View, ResultT &Result,
                        const APADriverOptions &Opts,
                        APADriverState &State, Printer &&PrintState) {
  const bool HasOutput = Opts.Stdout || !Opts.OutDir.empty();
  if (HasOutput && (Opts.DumpProfile || Opts.DumpExprs))
    dumpProfile(OS, View, Result, Opts.DumpExprs, State.HadSolveError);
  if (Opts.ProfileOnly || !HasOutput)
    return;
  lotus::dataflow_tool::printInstructionStates(
      OS, View, [&](Instruction *I) { PrintState(I, Result); });
}

template <typename Runner, typename Printer>
void runAnalysis(raw_ostream &OS, const FunctionView &View,
                 const elimination::EliminationOptions &ElimOpts,
                 const APADriverOptions &Opts, APADriverState &State,
                 Runner &&Run, Printer &&PrintState) {
  auto Result = Run(View.Function, ElimOpts);
  recordSolveStatus(Result, State.HadSolveError);
  dumpAnalysisResult(OS, View, Result, Opts, State,
                     std::forward<Printer>(PrintState));
}

template <typename ResultT, typename Printer>
void dumpInterproceduralResult(raw_ostream &OS, Module &M,
                               const ValueIdMap &ValueToId,
                               const ResultT &Result, Printer &&PrintState) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    lotus::dataflow_tool::emitFunctionHeader(OS, F);
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto Contexts = Result.contextsForInstruction(&I);
        std::sort(Contexts.begin(), Contexts.end(),
                  [&](const auto &Lhs, const auto &Rhs) {
                    return contextToString(Lhs.Ctx) < contextToString(Rhs.Ctx);
                  });
        if (Contexts.empty()) {
          OS << "  " << ValueToId.at(&I) << " IN: <no-context>\n";
          continue;
        }
        for (const auto &Key : Contexts) {
          OS << "  " << ValueToId.at(&I) << " IN [" << contextToString(Key.Ctx)
             << "]: ";
          PrintState(Key, Result);
          OS << "\n";
        }
      }
    }
  }
}

template <typename Runner, typename Printer>
void runInterproceduralAnalysis(raw_ostream &OS, Module &M,
                                Function &Entry,
                                const APADriverOptions &Opts,
                                APADriverState &State, Runner &&Run,
                                Printer &&PrintState) {
  auto Result = Run(Entry);
  recordSolveStatus(Result, State.HadSolveError);
  const bool HasOutput = Opts.Stdout || !Opts.OutDir.empty();
  if (HasOutput && Opts.DumpProfile) {
    printSolveMetadata(OS, Result, State.HadSolveError);
  }
  if (Opts.ProfileOnly || !HasOutput)
    return;
  const auto ValueToId = buildModuleValueIdMap(M);
  dumpInterproceduralResult(OS, M, ValueToId, Result,
                            [&](const auto &Key, const auto &Res) {
                              PrintState(Key, Res, ValueToId);
                            });
}

template <typename Runner, typename Printer>
void runInterSummaryAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                             const APADriverOptions &Opts,
                             APADriverState &State, Runner &&Run,
                             Printer &&PrintState) {
  auto Result = Run(Entry);
  recordSolveStatus(Result, State.HadSolveError);
  const auto ValueToId = buildModuleValueIdMap(M);
  if (Opts.DumpProfile) {
    printSolveMetadata(OS, Result, State.HadSolveError);
    emitInterSummaryDiagnostics(OS, Result);
  }
  dumpInterproceduralResult(OS, M, ValueToId, Result,
                            [&](const auto &Key, const auto &Res) {
                              PrintState(Key, Res, ValueToId);
                            });
}

void runInterSummaryReachable(raw_ostream &OS, Module &M, Function &Entry,
                              const APADriverOptions &Opts,
                              APADriverState &State) {
  auto SummaryOpts = buildInterSummaryOpts(Opts);
  runInterSummaryAnalysis(
      OS, M, Entry, Opts, State,
      [&](Function &F) {
        return elimination::runInterSummaryElimReachability(&F, nullptr, SummaryOpts);
      },
      [&](const auto &Key, const auto &Result, const auto &) {
        OS << (Result.IN(Key) ? "true" : "false");
      });
}

void runInterSummaryReachingDefinitions(raw_ostream &OS, Module &M,
                                        Function &Entry,
                                        const APADriverOptions &Opts,
                                        APADriverState &State) {
  auto SummaryOpts = buildInterSummaryOpts(Opts);
  runInterSummaryAnalysis(
      OS, M, Entry, Opts, State,
      [&](Function &F) {
        return elimination::runInterSummaryElimReachingDefinitions(
            &F, nullptr, nullptr, nullptr, SummaryOpts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

void runInterSummaryUninitialized(raw_ostream &OS, Module &M, Function &Entry,
                                  const APADriverOptions &Opts,
                                  APADriverState &State) {
  auto SummaryOpts = buildInterSummaryOpts(Opts);
  runInterSummaryAnalysis(
      OS, M, Entry, Opts, State,
      [&](Function &F) {
        return elimination::runInterSummaryElimUninitializedVariables(
            &F, nullptr, nullptr, nullptr, nullptr, SummaryOpts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

void runInterSummaryConstantPropagation(raw_ostream &OS, Module &M,
                                        Function &Entry,
                                        const APADriverOptions &Opts,
                                        APADriverState &State) {
  auto SummaryOpts = buildInterSummaryOpts(Opts);
  runInterSummaryAnalysis(
      OS, M, Entry, Opts, State,
      [&](Function &F) {
        return elimination::runInterSummaryElimConstantPropagation(
            &F, nullptr, nullptr, nullptr, nullptr, nullptr, SummaryOpts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueMap(
            OS, Result.IN(Key), ValueToId,
            [&](const elimination::ConstantPropagationValue &Value) {
              return formatValueLatticeElement(Value);
            });
      });
}

void runInterSummaryAvailableExpressions(raw_ostream &OS, Module &M,
                                         Function &Entry,
                                         const APADriverOptions &Opts,
                                         APADriverState &State) {
  auto SummaryOpts = buildInterSummaryOpts(Opts);
  runInterSummaryAnalysis(
      OS, M, Entry, Opts, State,
      [&](Function &F) {
        return elimination::runInterSummaryElimAvailableExpressions(&F, nullptr,
                                                                    SummaryOpts);
      },
      [&](const auto &Key, const auto &Result, const auto &) {
        std::vector<std::string> Expressions;
        for (const auto &Expression : Result.IN(Key))
          Expressions.push_back(formatExpressionKey(Expression));
        std::sort(Expressions.begin(), Expressions.end());
        for (std::size_t Index = 0; Index < Expressions.size(); ++Index) {
          if (Index != 0)
            OS << ",";
          OS << Expressions[Index];
        }
      });
}

void runInterSummaryLockset(raw_ostream &OS, Module &M, Function &Entry,
                            const APADriverOptions &Opts,
                            APADriverState &State) {
  auto SummaryOpts = buildInterSummaryOpts(Opts);
  runInterSummaryAnalysis(
      OS, M, Entry, Opts, State,
      [&](Function &F) {
        return elimination::runInterSummaryElimLockset(&F, nullptr, SummaryOpts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

void runInterSummaryNonNull(raw_ostream &OS, Module &M, Function &Entry,
                            const APADriverOptions &Opts,
                            APADriverState &State) {
  auto SummaryOpts = buildInterSummaryOpts(Opts);
  runInterSummaryAnalysis(
      OS, M, Entry, Opts, State,
      [&](Function &F) {
        return elimination::runInterSummaryElimNonNull(&F, nullptr, nullptr,
                                                       nullptr, SummaryOpts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

void runInterSummarySign(raw_ostream &OS, Module &M, Function &Entry,
                         const APADriverOptions &Opts,
                         APADriverState &State) {
  auto SummaryOpts = buildInterSummaryOpts(Opts);
  runInterSummaryAnalysis(
      OS, M, Entry, Opts, State,
      [&](Function &F) {
        return elimination::runInterSummaryElimSign(&F, nullptr, SummaryOpts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueMap(OS, Result.IN(Key), ValueToId,
                                             [](elimination::SignValue Value) {
                                               return formatSignValue(Value);
                                             });
      });
}

template <typename Runner>
void runSetIntraAnalysis(raw_ostream &OS, const FunctionView &View,
                         const elimination::EliminationOptions &ElimOpts,
                         const APADriverOptions &Opts, APADriverState &State,
                         Runner &&Run) {
  runAnalysis(OS, View, ElimOpts, Opts, State, std::forward<Runner>(Run),
              [&](Instruction *I, auto &Result) {
                lotus::dataflow_tool::formatValueSet(OS, Result.IN(I),
                                                     View.ValueToId);
              });
}

template <typename Runner>
void runBoolIntraAnalysis(raw_ostream &OS, const FunctionView &View,
                          const elimination::EliminationOptions &ElimOpts,
                          const APADriverOptions &Opts, APADriverState &State,
                          Runner &&Run) {
  runAnalysis(OS, View, ElimOpts, Opts, State, std::forward<Runner>(Run),
              [&](Instruction *I, auto &Result) {
                OS << (Result.IN(I) ? "true" : "false");
              });
}

template <typename Runner>
void runSetInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                         const APADriverOptions &Opts, APADriverState &State,
                         Runner &&Run) {
  runInterproceduralAnalysis(
      OS, M, Entry, Opts, State, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

template <typename Runner, typename Formatter>
void runMapInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                         const APADriverOptions &Opts, APADriverState &State,
                         Runner &&Run, Formatter &&FormatValue) {
  runInterproceduralAnalysis(
      OS, M, Entry, Opts, State, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueMap(OS, Result.IN(Key), ValueToId,
                                             FormatValue);
      });
}

template <typename Runner>
void runBoolInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                          const APADriverOptions &Opts, APADriverState &State,
                          Runner &&Run) {
  runInterproceduralAnalysis(
      OS, M, Entry, Opts, State, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &) {
        OS << (Result.IN(Key) ? "true" : "false");
      });
}

void runReachingDefinitions(raw_ostream &OS, const FunctionView &View,
                            const elimination::EliminationOptions &ElimOpts,
                            const APADriverOptions &Opts,
                            APADriverState &State) {
  const bool TranslApa = (Opts.Interp == "translapa");
  if (Opts.DumpProfile)
    OS << "  [interp] mode=" << (TranslApa ? "translapa" : "generic") << "\n";
  runSetIntraAnalysis(
      OS, View, ElimOpts, Opts, State,
      [TranslApa](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return TranslApa ? elimination::runIntraTranslApaReachingDefinitions(
                               &F, nullptr, EliminationOpts)
                         : elimination::runIntraElimReachingDefinitions(
                               &F, nullptr, EliminationOpts);
      });
}

void runUninitialized(raw_ostream &OS, const FunctionView &View,
                      const elimination::EliminationOptions &ElimOpts,
                      const APADriverOptions &Opts, APADriverState &State) {
  runSetIntraAnalysis(
      OS, View, ElimOpts, Opts, State,
      [](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return elimination::runIntraElimUninitializedVariables(&F, nullptr,
                                                               EliminationOpts);
      });
}

void runAffine(raw_ostream &OS, const FunctionView &View,
               const elimination::EliminationOptions &ElimOpts,
               const APADriverOptions &Opts, APADriverState &State) {
  runAnalysis(
      OS, View, ElimOpts, Opts, State,
      [](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return elimination::runIntraElimAffineEqualities(&F, EliminationOpts);
      },
      [&](Instruction *I, auto &Result) {
        OS << serializeAffine(Result.IN(I));
      });
}

void runConstantPropagation(raw_ostream &OS, const FunctionView &View,
                            const elimination::EliminationOptions &ElimOpts,
                            const APADriverOptions &Opts,
                            APADriverState &State) {
  runAnalysis(
      OS, View, ElimOpts, Opts, State,
      [](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return elimination::runIntraElimConstantPropagation(&F, nullptr, EliminationOpts);
      },
      [&](Instruction *I, auto &Result) {
        lotus::dataflow_tool::formatValueMap(
            OS, Result.IN(I), View.ValueToId,
            [&](const elimination::ConstantPropagationValue &Value) {
              return formatValueLatticeElement(Value);
            });
      });
}

void runAvailableExpressions(raw_ostream &OS, const FunctionView &View,
                             const elimination::EliminationOptions &ElimOpts,
                             const APADriverOptions &Opts,
                             APADriverState &State) {
  runAnalysis(
      OS, View, ElimOpts, Opts, State,
      [](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return elimination::runIntraElimAvailableExpressions(&F, nullptr, EliminationOpts);
      },
      [&](Instruction *I, auto &Result) {
        std::vector<std::string> Exprs;
        for (const auto &Expr : Result.IN(I))
          Exprs.push_back(formatExpressionKey(Expr));
        std::sort(Exprs.begin(), Exprs.end());
        for (size_t Index = 0; Index < Exprs.size(); ++Index) {
          if (Index)
            OS << ",";
          OS << Exprs[Index];
        }
      });
}

void runLockset(raw_ostream &OS, const FunctionView &View,
                const elimination::EliminationOptions &ElimOpts,
                const APADriverOptions &Opts, APADriverState &State) {
  runSetIntraAnalysis(
      OS, View, ElimOpts, Opts, State,
      [](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return elimination::runIntraElimLockset(&F, EliminationOpts);
      });
}

void runNonNull(raw_ostream &OS, const FunctionView &View,
                const elimination::EliminationOptions &ElimOpts,
                const APADriverOptions &Opts, APADriverState &State) {
  runSetIntraAnalysis(
      OS, View, ElimOpts, Opts, State,
      [](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return elimination::runIntraElimNonNull(&F, EliminationOpts);
      });
}

void runSign(raw_ostream &OS, const FunctionView &View,
             const elimination::EliminationOptions &ElimOpts,
             const APADriverOptions &Opts, APADriverState &State) {
  runAnalysis(
      OS, View, ElimOpts, Opts, State,
      [](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return elimination::runIntraElimSign(&F, EliminationOpts);
      },
      [&](Instruction *I, auto &Result) {
        lotus::dataflow_tool::formatValueMap(OS, Result.IN(I), View.ValueToId,
                                             [](elimination::SignValue Value) {
                                               return formatSignValue(Value);
                                             });
      });
}

void runReachable(raw_ostream &OS, const FunctionView &View,
                  const elimination::EliminationOptions &ElimOpts,
                  const APADriverOptions &Opts, APADriverState &State) {
  const bool TranslApa = (Opts.Interp == "translapa");
  if (Opts.DumpProfile)
    OS << "  [interp] mode=" << (TranslApa ? "translapa" : "generic") << "\n";
  runBoolIntraAnalysis(
      OS, View, ElimOpts, Opts, State,
      [TranslApa](Function &F, const elimination::EliminationOptions &EliminationOpts) {
        return TranslApa ? elimination::runIntraTranslApaReachability(&F, EliminationOpts)
                         : elimination::runIntraElimReachability(&F, EliminationOpts);
      });
}

void runInterReachingDefinitions(raw_ostream &OS, Module &M, Function &Entry,
                                 const APADriverOptions &Opts,
                                 APADriverState &State) {
  if (Opts.InterEngine == "expanded") {
    runInterSummaryReachingDefinitions(OS, M, Entry, Opts, State);
    return;
  }
  runSetInterAnalysis(OS, M, Entry, Opts, State, [&Opts](Function &F) {
    return elimination::runInterElimReachingDefinitions(
        &F, nullptr, nullptr, nullptr, buildElimOpts(Opts));
  });
}

void runInterUninitialized(raw_ostream &OS, Module &M, Function &Entry,
                           const APADriverOptions &Opts,
                           APADriverState &State) {
  if (Opts.InterEngine == "expanded") {
    runInterSummaryUninitialized(OS, M, Entry, Opts, State);
    return;
  }
  runSetInterAnalysis(OS, M, Entry, Opts, State, [&Opts](Function &F) {
    return elimination::runInterElimUninitializedVariables(
        &F, nullptr, nullptr, nullptr, nullptr, buildElimOpts(Opts));
  });
}

void runInterConstantPropagation(raw_ostream &OS, Module &M, Function &Entry,
                                 const APADriverOptions &Opts,
                                 APADriverState &State) {
  if (Opts.InterEngine == "expanded") {
    runInterSummaryConstantPropagation(OS, M, Entry, Opts, State);
    return;
  }
  runMapInterAnalysis(
      OS, M, Entry, Opts, State,
      [&Opts](Function &F) {
        return elimination::runInterElimConstantPropagation(
            &F, nullptr, nullptr, nullptr, nullptr, nullptr, buildElimOpts(Opts));
      },
      [&](const elimination::ConstantPropagationValue &Value) {
        return formatValueLatticeElement(Value);
      });
}

void runInterAvailableExpressions(raw_ostream &OS, Module &M, Function &Entry,
                                  const APADriverOptions &Opts,
                                  APADriverState &State) {
  if (Opts.InterEngine == "expanded") {
    runInterSummaryAvailableExpressions(OS, M, Entry, Opts, State);
    return;
  }
  runInterproceduralAnalysis(
      OS, M, Entry, Opts, State,
      [&Opts](Function &F) {
        return elimination::runInterElimAvailableExpressions(&F, nullptr,
                                                             buildElimOpts(Opts));
      },
      [&](const auto &Key, const auto &Result, const auto &) {
        std::vector<std::string> Expressions;
        for (const auto &Expression : Result.IN(Key))
          Expressions.push_back(formatExpressionKey(Expression));
        std::sort(Expressions.begin(), Expressions.end());
        for (std::size_t Index = 0; Index < Expressions.size(); ++Index) {
          if (Index != 0)
            OS << ",";
          OS << Expressions[Index];
        }
      });
}

void runInterLockset(raw_ostream &OS, Module &M, Function &Entry,
                     const APADriverOptions &Opts, APADriverState &State) {
  if (Opts.InterEngine == "expanded") {
    runInterSummaryLockset(OS, M, Entry, Opts, State);
    return;
  }
  runSetInterAnalysis(OS, M, Entry, Opts, State, [&Opts](Function &F) {
    return elimination::runInterElimLockset(&F, nullptr, buildElimOpts(Opts));
  });
}

void runInterNonNull(raw_ostream &OS, Module &M, Function &Entry,
                     const APADriverOptions &Opts, APADriverState &State) {
  if (Opts.InterEngine == "expanded") {
    runInterSummaryNonNull(OS, M, Entry, Opts, State);
    return;
  }
  runSetInterAnalysis(OS, M, Entry, Opts, State, [&Opts](Function &F) {
    return elimination::runInterElimNonNull(&F, nullptr, nullptr, nullptr,
                                            buildElimOpts(Opts));
  });
}

void runInterSign(raw_ostream &OS, Module &M, Function &Entry,
                  const APADriverOptions &Opts, APADriverState &State) {
  if (Opts.InterEngine == "expanded") {
    runInterSummarySign(OS, M, Entry, Opts, State);
    return;
  }
  runMapInterAnalysis(
      OS, M, Entry, Opts, State,
      [&Opts](Function &F) {
        return elimination::runInterElimSign(&F, nullptr, buildElimOpts(Opts));
      },
      [](elimination::SignValue Value) { return formatSignValue(Value); });
}

void runInterReachable(raw_ostream &OS, Module &M, Function &Entry,
                       const APADriverOptions &Opts, APADriverState &State) {
  if (Opts.InterEngine == "modular") {
    auto SummaryOpts = buildInterSummaryOpts(Opts);
    runInterSummaryAnalysis(
        OS, M, Entry, Opts, State,
        [&](Function &F) {
          return elimination::runModularInterReachability(&F, nullptr, SummaryOpts);
        },
        [&](const auto &Key, const auto &Result, const auto &) {
          OS << (Result.IN(Key) ? "true" : "false");
        });
    return;
  }
  if (Opts.InterEngine == "expanded") {
    runInterSummaryReachable(OS, M, Entry, Opts, State);
    return;
  }
  runBoolInterAnalysis(OS, M, Entry, Opts, State, [&Opts](Function &F) {
    return elimination::runInterElimReachability(&F, nullptr, buildElimOpts(Opts));
  });
}

void runInterAffine(raw_ostream &OS, Module &M, Function &,
                    const APADriverOptions &Opts, APADriverState &State) {
  elimination::InterAffineEqualitiesOptions Options;
  Options.vocabulary = elimination::InterAffineVocabularyMode::ObservableSlice;
  Options.verbose = false;
  Options.maxTrackedValues = Opts.AffineMaxTracked;
  Options.ordering = parseOrderingPolicy(Opts.Ordering);
  Options.order = buildOrderOpts(Opts);
  auto Result = elimination::runInterElimAffineEqualities(M, Options);
  State.HadSolveError |= Result.status == elimination::SolveStatus::InvalidProblem ||
                         Result.status == elimination::SolveStatus::NonConvergentStar;
  if (Opts.DumpProfile) {
    emitOrderingDiagnostics(OS, Result.diagnostics.ordering);
    OS << "  [semantic-star] time_ns=" << Result.diagnostics.semantic_star_time_ns
       << ", iterations=" << Result.diagnostics.star_iterations_total << "\n";
    OS << "  [profile] tracked_values=" << Result.trackedValues
       << ", summaries=" << Result.summaries.size()
       << ", block_relations=" << Result.blockRelations.size()
       << ", status=" << toString(Result.status) << "\n";
  }
}

const AnalysisHandler Handlers[] = {
    {"reaching_defs", false, &runReachingDefinitions, nullptr},
    {"uninitialized", false, &runUninitialized, nullptr},
    {"constant_prop", false, &runConstantPropagation, nullptr},
    {"available_exprs", false, &runAvailableExpressions, nullptr},
    {"affine", false, &runAffine, nullptr},
    {"reachable", false, &runReachable, nullptr},
    {"lockset", false, &runLockset, nullptr},
    {"nonnull", false, &runNonNull, nullptr},
    {"sign", false, &runSign, nullptr},
    {"inter_reaching_defs", true, nullptr, &runInterReachingDefinitions},
    {"inter_uninitialized", true, nullptr, &runInterUninitialized},
    {"inter_constant_prop", true, nullptr, &runInterConstantPropagation},
    {"inter_available_exprs", true, nullptr, &runInterAvailableExpressions},
    {"inter_reachable", true, nullptr, &runInterReachable},
    {"inter_lockset", true, nullptr, &runInterLockset},
    {"inter_nonnull", true, nullptr, &runInterNonNull},
    {"inter_sign", true, nullptr, &runInterSign},
    {"inter_affine", true, nullptr, &runInterAffine},
};

}

ArrayRef<AnalysisHandler> getAvailableAnalyses() {
  return llvm::makeArrayRef(Handlers);
}

const AnalysisHandler *findAnalysisHandler(StringRef Name) {
  for (const auto &H : Handlers) {
    if (H.Name == Name)
      return &H;
  }
  return nullptr;
}

int runApaDriver(Module &M, raw_ostream &OS, const APADriverOptions &Opts) {
  std::vector<const AnalysisHandler *> Clients;
  {
    std::stringstream SS(Opts.Analysis);
    std::string Name;
    while (std::getline(SS, Name, ',')) {
      if (Name.empty())
        continue;
      const auto *H = findAnalysisHandler(Name);
      if (!H) {
        errs() << "error: unknown elimination analysis '" << Name << "'\n";
        return 1;
      }
      Clients.push_back(H);
    }
  }
  if (Clients.empty()) {
    errs() << "error: no analysis selected\n";
    return 1;
  }

  const auto ElimOpts = buildElimOpts(Opts);
  OS << "[elim] clients=" << Opts.Analysis << ", method=" << Opts.ElimMethod
     << ", ordering=" << Opts.Ordering << ", ean=" << (Opts.Ean ? "on" : "off")
     << ", ean_laws=" << Opts.EanLaws
     << ", ean_min_nodes=" << Opts.EanMinNodes << ", interp_repeat=" << Opts.InterpRepeat
     << ", max_func_insts=" << Opts.MaxFuncInsts << "\n";

  APADriverState State;
  const bool AnyModule =
      std::any_of(Clients.begin(), Clients.end(),
                  [](const AnalysisHandler *H) { return H->ModuleScoped; });
  if (AnyModule) {
    if (Clients.size() != 1) {
      errs() << "error: module-scoped analyses must be run one at a time\n";
      return 1;
    }
    Function *Entry = M.getFunction(Opts.EntryFunction);
    if (Entry == nullptr || Entry->isDeclaration()) {
      errs() << "error: entry function '" << Opts.EntryFunction
             << "' not found or is a declaration\n";
      return 1;
    }
    Clients.front()->RunModule(OS, M, *Entry, Opts, State);
  } else {
    std::size_t Skipped = 0;
    lotus::dataflow_tool::forEachDefinedFunction(
        M, OS, [&](const FunctionView &View) {
          if (Opts.MaxFuncInsts != 0 && View.OrderedInsts.size() > Opts.MaxFuncInsts) {
            OS << "  [skipped] reason=too_large insts="
               << View.OrderedInsts.size() << "\n";
            ++Skipped;
            return;
          }
          for (const AnalysisHandler *H : Clients) {
            OS << "  [client:" << H->Name << "]\n";
            H->RunFunction(OS, View, ElimOpts, Opts, State);
          }
        });
    OS << "[summary] skipped_functions=" << Skipped << "\n";
  }

  return State.HadSolveError ? 1 : 0;
}

}
