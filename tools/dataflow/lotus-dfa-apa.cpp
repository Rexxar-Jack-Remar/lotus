/*
 * lotus-dfa-apa
 *
 * Dataflow testing tool: APA (Algebraic Program Analysis) engine.
 */

#include "llvm/IR/CFG.h"
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

#include "Dataflow/APA/Clients/LLVM/Intra/AvailableExpressions.h"
#include "Dataflow/APA/Clients/LLVM/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Clients/LLVM/Intra/LiveVariables.h"
#include "Dataflow/APA/Clients/LLVM/Intra/Reachability.h"
#include "Dataflow/APA/Clients/LLVM/Intra/ReachingDefinitions.h"
#include "Dataflow/APA/Clients/LLVM/Intra/UninitializedVariables.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
static cl::opt<std::string> ElimMethodOpt(
    "elim-method",
    cl::desc("Elimination solver method: state|adt-simple|adt-delayed"),
    cl::init("state"));
static cl::opt<bool>
    DumpProfileOpt("dump-profile",
                   cl::desc("Dump solver and path-expression profiling data"),
                   cl::init(false));
static cl::opt<bool>
    DumpExprsOpt("dump-exprs",
                 cl::desc("Dump per-instruction path-expression summaries"),
                 cl::init(false));

namespace {

using InstructionExprFactory = elimination::PathExprFactory<Instruction *>;
using InstructionExprRef = InstructionExprFactory::Ref;

const char *toString(elimination::EliminationMethod M) {
  switch (M) {
  case elimination::EliminationMethod::StateElimination:
    return "state";
  case elimination::EliminationMethod::ADTSimple:
    return "adt-simple";
  case elimination::EliminationMethod::ADTDelayed:
    return "adt-delayed";
  }
  return "unknown";
}

const char *toString(elimination::SolveStatus S) {
  switch (S) {
  case elimination::SolveStatus::Ok:
    return "ok";
  case elimination::SolveStatus::FallbackToState:
    return "fallback-to-state";
  case elimination::SolveStatus::NonConvergentStar:
    return "non-convergent-star";
  case elimination::SolveStatus::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

const char *toString(elimination::FallbackReason R) {
  switch (R) {
  case elimination::FallbackReason::None:
    return "none";
  case elimination::FallbackReason::ADTRejected:
    return "adt-rejected";
  case elimination::FallbackReason::InvalidProblem:
    return "invalid-problem";
  }
  return "unknown";
}

elimination::EliminationOptions getElimOptions() {
  elimination::EliminationOptions Opts;
  if (ElimMethodOpt == "adt-simple")
    Opts.Method = elimination::EliminationMethod::ADTSimple;
  else if (ElimMethodOpt == "adt-delayed")
    Opts.Method = elimination::EliminationMethod::ADTDelayed;
  else
    Opts.Method = elimination::EliminationMethod::StateElimination;
  return Opts;
}

void buildValueIds(Function *F,
                   std::unordered_map<const Value *, std::string> &ValueToId,
                   std::vector<Instruction *> &OrderedInsts) {
  unsigned ArgIdx = 0;
  for (auto &Arg : F->args())
    ValueToId[&Arg] = "arg" + std::to_string(ArgIdx++);
  unsigned InstIdx = 0;
  for (auto &BB : *F)
    for (auto &I : BB) {
      OrderedInsts.push_back(&I);
      ValueToId[&I] = "i" + std::to_string(InstIdx++);
    }
}

template <typename T>
void formatValueSet(
    raw_ostream &OS, const std::set<T> &S,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
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

std::string formatValueLatticeElement(const ValueLatticeElement &Val) {
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

struct CFGStats final {
  size_t Arguments = 0;
  size_t Blocks = 0;
  size_t Instructions = 0;
  size_t Edges = 0;
  size_t BranchingBlocks = 0;
  size_t MaxSuccessors = 0;
  size_t PhiNodes = 0;
  size_t Calls = 0;
  size_t Returns = 0;
  size_t Unreachable = 0;
};

CFGStats collectCFGStats(const Function &F) {
  CFGStats Stats;
  Stats.Arguments = F.arg_size();
  for (const auto &BB : F) {
    ++Stats.Blocks;
    Stats.Instructions += BB.size();
    for (const auto &I : BB) {
      if (isa<PHINode>(I))
        ++Stats.PhiNodes;
      if (isa<CallBase>(I))
        ++Stats.Calls;
      if (isa<ReturnInst>(I))
        ++Stats.Returns;
      if (isa<UnreachableInst>(I))
        ++Stats.Unreachable;
    }
    const size_t Succs = succ_size(&BB);
    Stats.Edges += Succs;
    Stats.MaxSuccessors = std::max(Stats.MaxSuccessors, Succs);
    if (Succs > 1)
      ++Stats.BranchingBlocks;
  }
  return Stats;
}

struct ExprProfile final {
  size_t UniqueNodes = 0;
  size_t SharedRefs = 0;
  size_t MaxDepth = 0;
  size_t ZeroNodes = 0;
  size_t OneNodes = 0;
  size_t AtomNodes = 0;
  size_t UnionNodes = 0;
  size_t ConcatNodes = 0;
  size_t StarNodes = 0;
};

void collectExprProfileImpl(const InstructionExprRef &Expr, size_t Depth,
                            std::unordered_set<const void *> &Visited,
                            ExprProfile &Profile) {
  if (!Expr)
    return;

  Profile.MaxDepth = std::max(Profile.MaxDepth, Depth);
  if (!Visited.insert(Expr.get()).second) {
    ++Profile.SharedRefs;
    return;
  }

  ++Profile.UniqueNodes;
  switch (Expr->K) {
  case InstructionExprFactory::Kind::Zero:
    ++Profile.ZeroNodes;
    return;
  case InstructionExprFactory::Kind::One:
    ++Profile.OneNodes;
    return;
  case InstructionExprFactory::Kind::Atom:
    ++Profile.AtomNodes;
    return;
  case InstructionExprFactory::Kind::Union:
    ++Profile.UnionNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case InstructionExprFactory::Kind::Concat:
    ++Profile.ConcatNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case InstructionExprFactory::Kind::Star:
    ++Profile.StarNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    return;
  }
}

ExprProfile collectExprProfile(const InstructionExprRef &Expr) {
  ExprProfile Profile;
  std::unordered_set<const void *> Visited;
  collectExprProfileImpl(Expr, 1, Visited, Profile);
  return Profile;
}

std::string formatTransfer(
    const Instruction *I,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  if (!I)
    return "null";
  auto It = ValueToId.find(I);
  if (It != ValueToId.end())
    return It->second;
  return "inst";
}

void formatPathExpr(
    raw_ostream &OS, const InstructionExprRef &Expr,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
  if (!Expr) {
    OS << "null";
    return;
  }

  switch (Expr->K) {
  case InstructionExprFactory::Kind::Zero:
    OS << "zero";
    return;
  case InstructionExprFactory::Kind::One:
    OS << "one";
    return;
  case InstructionExprFactory::Kind::Atom:
    OS << "atom("
       << formatTransfer(Expr->Transfer ? *Expr->Transfer : nullptr, ValueToId)
       << ")";
    return;
  case InstructionExprFactory::Kind::Union:
    OS << "union(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case InstructionExprFactory::Kind::Concat:
    OS << "concat(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case InstructionExprFactory::Kind::Star:
    OS << "star(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ")";
    return;
  }
}

template <typename ResultT>
void printSolveMetadata(raw_ostream &OS, const ResultT &Result) {
  if (!Result.hasSolveMetadata())
    return;
  const auto &Diag = Result.solveDiagnostics();
  OS << "  [solver] status=" << toString(Result.solveStatus())
     << ", requested=" << toString(Diag.requested_method)
     << ", executed=" << toString(Diag.executed_method)
     << ", used_adt=" << (Diag.used_adt ? "true" : "false")
     << ", fallback=" << toString(Diag.fallback_reason)
     << ", star_iters=" << Diag.star_iterations_total
     << ", max_star_hit=" << (Diag.max_star_hit ? "true" : "false") << "\n";
}

template <typename ResultT>
void dumpProfile(
    raw_ostream &OS, const Function &F, const ResultT &Result,
    const std::unordered_map<const Value *, std::string> &ValueToId,
    const std::vector<Instruction *> &OrderedInsts,
    std::chrono::microseconds Elapsed) {
  const auto CFG = collectCFGStats(F);
  OS << "  [cfg] args=" << CFG.Arguments << ", blocks=" << CFG.Blocks
     << ", insts=" << CFG.Instructions << ", edges=" << CFG.Edges
     << ", branching_blocks=" << CFG.BranchingBlocks
     << ", max_succs=" << CFG.MaxSuccessors << ", phis=" << CFG.PhiNodes
     << ", calls=" << CFG.Calls << ", returns=" << CFG.Returns
     << ", unreachable=" << CFG.Unreachable
     << ", elapsed_us=" << Elapsed.count() << "\n";
  printSolveMetadata(OS, Result);

  size_t NodesWithExpr = 0;
  size_t MissingExpr = 0;
  size_t TotalUniqueNodes = 0;
  size_t TotalSharedRefs = 0;
  size_t TotalStars = 0;
  size_t TotalUnions = 0;
  size_t TotalConcats = 0;
  size_t MaxExprNodes = 0;
  size_t MaxExprDepth = 0;
  std::string MaxExprInst = "none";
  std::string DeepestExprInst = "none";

  for (auto *I : OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    if (!Expr) {
      ++MissingExpr;
      continue;
    }

    ++NodesWithExpr;
    const auto Profile = collectExprProfile(Expr);
    TotalUniqueNodes += Profile.UniqueNodes;
    TotalSharedRefs += Profile.SharedRefs;
    TotalStars += Profile.StarNodes;
    TotalUnions += Profile.UnionNodes;
    TotalConcats += Profile.ConcatNodes;
    if (Profile.UniqueNodes > MaxExprNodes) {
      MaxExprNodes = Profile.UniqueNodes;
      MaxExprInst = ValueToId.at(I);
    }
    if (Profile.MaxDepth > MaxExprDepth) {
      MaxExprDepth = Profile.MaxDepth;
      DeepestExprInst = ValueToId.at(I);
    }
  }

  OS << "  [expr-profile] with_expr=" << NodesWithExpr
     << ", missing_expr=" << MissingExpr
     << ", total_unique_nodes=" << TotalUniqueNodes
     << ", total_shared_refs=" << TotalSharedRefs
     << ", total_unions=" << TotalUnions << ", total_concats=" << TotalConcats
     << ", total_stars=" << TotalStars << ", max_nodes=" << MaxExprNodes << "@"
     << MaxExprInst << ", max_depth=" << MaxExprDepth << "@" << DeepestExprInst
     << "\n";

  if (!DumpExprsOpt)
    return;

  for (auto *I : OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    OS << "  [expr] " << ValueToId.at(I);
    if (!Expr) {
      OS << " missing\n";
      continue;
    }
    const auto Profile = collectExprProfile(Expr);
    OS << " nodes=" << Profile.UniqueNodes << ", depth=" << Profile.MaxDepth
       << ", atoms=" << Profile.AtomNodes << ", unions=" << Profile.UnionNodes
       << ", concats=" << Profile.ConcatNodes << ", stars=" << Profile.StarNodes
       << ", shared_refs=" << Profile.SharedRefs << ", expr=";
    formatPathExpr(OS, Expr, ValueToId);
    OS << "\n";
  }
}

template <typename ValueType>
void formatConstPropMap(
    raw_ostream &OS, const std::unordered_map<const Value *, ValueType> &M,
    const std::unordered_map<const Value *, std::string> &ValueToId) {
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

void dumpFunctionAnalysis(raw_ostream &OS, Function &F,
                          const std::string &Analysis,
                          const elimination::EliminationOptions &ElimOpts) {
  std::unordered_map<const Value *, std::string> ValueToId;
  std::vector<Instruction *> OrderedInsts;
  buildValueIds(&F, ValueToId, OrderedInsts);
  OS << "FUNC " << F.getName().str() << "\n";

  if (Analysis == "liveness") {
    const auto Start = std::chrono::steady_clock::now();
    auto Result = elimination::runIntraElimLiveVariables(&F, ElimOpts);
    const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - Start);
    if (DumpProfileOpt || DumpExprsOpt)
      dumpProfile(OS, F, Result, ValueToId, OrderedInsts, Elapsed);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      formatValueSet(OS, Result.IN(I), ValueToId);
      OS << "\n";
    }
  } else if (Analysis == "reaching_defs") {
    const auto Start = std::chrono::steady_clock::now();
    auto Result =
        elimination::runIntraElimReachingDefinitions(&F, nullptr, ElimOpts);
    const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - Start);
    if (DumpProfileOpt || DumpExprsOpt)
      dumpProfile(OS, F, Result, ValueToId, OrderedInsts, Elapsed);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      formatValueSet(OS, Result.IN(I), ValueToId);
      OS << "\n";
    }
  } else if (Analysis == "uninitialized") {
    const auto Start = std::chrono::steady_clock::now();
    auto Result =
        elimination::runIntraElimUninitVariables(&F, nullptr, ElimOpts);
    const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - Start);
    if (DumpProfileOpt || DumpExprsOpt)
      dumpProfile(OS, F, Result, ValueToId, OrderedInsts, Elapsed);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      formatValueSet(OS, Result.IN(I), ValueToId);
      OS << "\n";
    }
  } else if (Analysis == "constant_prop") {
    const auto Start = std::chrono::steady_clock::now();
    auto Result =
        elimination::runIntraElimConstantPropagation(&F, nullptr, ElimOpts);
    const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - Start);
    if (DumpProfileOpt || DumpExprsOpt)
      dumpProfile(OS, F, Result, ValueToId, OrderedInsts, Elapsed);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      formatConstPropMap(OS, Result.IN(I), ValueToId);
      OS << "\n";
    }
  } else if (Analysis == "available_exprs") {
    const auto Start = std::chrono::steady_clock::now();
    auto Result =
        elimination::runIntraElimAvailableExpressions(&F, nullptr, ElimOpts);
    const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - Start);
    if (DumpProfileOpt || DumpExprsOpt)
      dumpProfile(OS, F, Result, ValueToId, OrderedInsts, Elapsed);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      std::vector<std::string> exprs;
      for (const auto &expr : Result.IN(I))
        exprs.push_back(formatExpressionKey(expr));
      std::sort(exprs.begin(), exprs.end());
      for (size_t i = 0; i < exprs.size(); ++i) {
        if (i)
          OS << ",";
        OS << exprs[i];
      }
      OS << "\n";
    }
  } else if (Analysis == "reachable") {
    const auto Start = std::chrono::steady_clock::now();
    auto Result = elimination::runIntraElimReachable(&F, ElimOpts);
    const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - Start);
    if (DumpProfileOpt || DumpExprsOpt)
      dumpProfile(OS, F, Result, ValueToId, OrderedInsts, Elapsed);
    for (auto *I : OrderedInsts) {
      OS << "  " << ValueToId.at(I) << " IN: ";
      OS << (Result.IN(I) ? "true" : "false");
      OS << "\n";
    }
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
  const auto ElimOpts = getElimOptions();

  OS << "[elim:" << AnalysisOpt << "]\n";

  for (auto &F : *M)
    if (!F.isDeclaration())
      dumpFunctionAnalysis(OS, F, AnalysisOpt, ElimOpts);

  return 0;
}
