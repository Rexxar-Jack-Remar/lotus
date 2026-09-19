/*
 * lotus-dfa-apa
 *
 * Dataflow testing tool: APA (Algebraic Program Analysis) engine.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
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
#include "Dataflow/APA/Solver/Ordering/Selector.h"
#include "Dataflow/Tooling/ToolSupport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h must follow windows.h
#include <psapi.h>
// windows.h defines IN/OUT as empty SAL macros, which would mangle the solver
// result accessor Result.IN(...). Drop them.
#undef IN
#undef OUT
#else
#include <sys/resource.h>
#endif

using namespace llvm;

static bool HadSolveError = false;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                          cl::Required);
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<bool> StdoutOpt(
    "stdout",
    cl::desc("Write analysis results to stdout when --out-dir is not set"),
    cl::init(false));
static cl::opt<std::string> AnalysisOpt(
    "analysis",
    cl::desc("Analysis: reachable (default), reaching_defs, uninitialized, "
             "constant_prop, available_exprs, affine, lockset, nonnull, sign, "
             "inter_reaching_defs, inter_uninitialized, "
             "inter_constant_prop, inter_available_exprs, inter_reachable, "
             "inter_lockset, inter_nonnull, inter_sign, inter_affine"),
    cl::init("reachable"));
static cl::opt<std::string>
    EntryFunctionOpt("entry-function",
                     cl::desc("Entry function for interprocedural analyses"),
                     cl::init("main"));
static cl::opt<std::string> ElimMethodOpt(
    "elim-method",
    cl::desc("Elimination solver method: state|adt-simple|adt-delayed"),
    cl::init("state"));
static cl::opt<bool>
    DumpProfileOpt("dump-profile",
                   cl::desc("Dump solver and path-expression profiling data"),
                   cl::init(false));
static cl::opt<bool> ProfileOnlyOpt(
    "profile-only",
    cl::desc("Emit profiling data without per-instruction facts"),
    cl::init(false));
static cl::opt<bool>
    DumpExprsOpt("dump-exprs",
                 cl::desc("Dump per-instruction path-expression summaries"),
                 cl::init(false));
static cl::opt<unsigned> AffineMaxTrackedOpt(
    "affine-max-tracked",
    cl::desc("Maximum values in the inter-affine observable slice (0 = no "
             "limit)"),
    cl::init(32));

// --- EAN / Order (evaluation) configuration ---------------------------------
static cl::opt<std::string> OrderingOpt(
    "ordering",
    cl::desc("Pivot order: "
             "default|cost-aware|structural|expr-aware|star-risk|hybrid|"
             "rpo|random|min-degree|min-fill|explicit"),
    cl::init("default"));
static cl::opt<double>
    OrderStructCap("order-struct-cap", cl::init(64.0),
                   cl::desc("Structural normalization cap (>0)"));
static cl::opt<double>
    OrderExprCap("order-expr-cap", cl::init(4096.0),
                 cl::desc("Expression normalization cap (>0)"));
static cl::opt<double>
    OrderStarCap("order-star-cap", cl::init(256.0),
                 cl::desc("Iteration normalization cap (>0)"));
static cl::opt<unsigned>
    OrderSizeCap("order-size-cap", cl::init(1024),
                 cl::desc("Cached DAG-size cap (0 = exact)"));
static cl::opt<bool>
    OrderFullRescore("order-full-rescore", cl::init(false),
                     cl::desc("Rescore all live candidates (ablation)"));
static cl::opt<bool>
    OrderTrace("order-trace", cl::init(false),
               cl::desc("Emit candidate refreshes and elimination steps"));
static cl::opt<bool>
    OrderSparse("order-sparse", cl::init(false),
                cl::desc("Use common sparse engine for baseline comparisons"));
static cl::opt<unsigned long long>
    OrderSeed("order-seed", cl::init(0),
              cl::desc("Seed for randomized pivot order"));
static cl::opt<std::string>
    OrderExplicit("order-explicit", cl::init(""),
                  cl::desc("Comma-separated local region indices"));
static cl::opt<bool> EanOpt("ean", cl::desc("Run the EAN normalizer post-pass"),
                            cl::init(false));
static cl::opt<bool>
    GreedyOpt("greedy",
              cl::desc("Run the Greedy one-pass simplifier post-pass"),
              cl::init(false));
static cl::opt<bool> EanMonotoneOpt(
    "ean-monotone",
    cl::desc("EAN returns the input if its output has more nodes"),
    cl::init(false));
static cl::opt<std::string> EanLawsOpt("ean-laws",
                                       cl::desc("EAN law profile: safe|kleene"),
                                       cl::init("safe"));
static cl::opt<std::string>
    EanCostOpt("ean-cost",
               cl::desc("EAN extraction cost: uniform|dag (dag counts edges "
                        "≈ exported factory nodes)"),
               cl::init("uniform"));
static cl::opt<unsigned>
    EanRoundLimit("ean-round-limit",
                  cl::desc("EAN saturation round budget (0 = unbounded)"),
                  cl::init(0));
static cl::opt<unsigned>
    EanNodeLimit("ean-node-limit",
                 cl::desc("EAN e-node budget (0 = unbounded)"), cl::init(0));
static cl::opt<double>
    EanTimeLimit("ean-time-limit",
                 cl::desc("EAN wall-clock budget in seconds (0 = unbounded)"),
                 cl::init(0.0));
static cl::opt<bool>
    MeasurePeakOpt("measure-peak",
                   cl::desc("Record peak construction nodes (slower)"),
                   cl::init(false));
static cl::opt<unsigned>
    MaxFuncInsts("max-func-insts",
                 cl::desc("Skip functions with more instructions (0 = no cap)"),
                 cl::init(0));
static cl::opt<unsigned>
    EanMinNodes("ean-min-nodes",
                cl::desc("Invocation gate: run EAN only if raw batch has >= N "
                         "unique DAG nodes (0 = always)"),
                cl::init(0));
static cl::opt<unsigned>
    InterpRepeat("interp-repeat",
                 cl::desc("Interpret each summary N times (amortization, RQ2)"),
                 cl::init(1));
static cl::opt<unsigned>
    RepeatOpt("repeat", cl::desc("Measured runs per function (timing median)"),
              cl::init(1));
static cl::opt<unsigned>
    WarmupOpt("warmup", cl::desc("Warmup runs per function before measuring"),
              cl::init(0));
static cl::opt<std::string>
    InterEngineOpt("inter-engine",
                   cl::desc("Interprocedural model: context|expanded|modular"),
                   cl::init("context"));
static cl::opt<bool> MemoInterpOpt(
    "memo-interp",
    cl::desc("Affine client only: memoizing transformer interpreter "
             "(eval cost proportional to unique DAG nodes, not tree size)"),
    cl::init(false));
static cl::opt<std::string> InterpOpt(
    "interp",
    cl::desc(
        "Path-expression interpreter: generic (framework eval) | translapa "
        "(closed-form Gen/Kill semiring baseline). Applies to the reachable "
        "and reachdef clients."),
    cl::init("generic"));

namespace {

using lotus::dataflow_tool::FunctionView;
using lotus::dataflow_tool::ValueIdMap;
using InstructionExprFactory = elimination::PathExprFactory<Instruction *>;
using InstructionExprRef = InstructionExprFactory::Ref;

// Aggregated stage timings (microseconds) across the measured repeats.
struct Timings final {
  std::uint64_t gen = 0;
  std::uint64_t norm = 0;
  std::uint64_t interp = 0;
  std::uint64_t end2end = 0;
  unsigned runs = 1;
};

std::uint64_t medianOf(std::vector<std::uint64_t> V) {
  if (V.empty())
    return 0;
  std::sort(V.begin(), V.end());
  return V[V.size() / 2];
}

// Peak resident memory of this process in KiB (Table VII Peak RSS).
std::uint64_t peakRssKb() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS PMC;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &PMC, sizeof(PMC)))
    return static_cast<std::uint64_t>(PMC.PeakWorkingSetSize) / 1024;
  return 0;
#elif defined(__APPLE__)
  struct rusage RU;
  if (getrusage(RUSAGE_SELF, &RU) == 0)
    return static_cast<std::uint64_t>(RU.ru_maxrss) / 1024; // bytes on macOS
  return 0;
#else
  struct rusage RU;
  if (getrusage(RUSAGE_SELF, &RU) == 0)
    return static_cast<std::uint64_t>(RU.ru_maxrss); // KiB on Linux
  return 0;
#endif
}

elimination::OrderingPolicy parseOrderingPolicy() {
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
  throw std::invalid_argument("unknown APA ordering policy: " + OrderingOpt);
}

elimination::OrderPolicyOptions buildOrderOpts() {
  elimination::OrderPolicyOptions Opts;
  Opts.StructuralCap = OrderStructCap;
  Opts.ExpressionCap = OrderExprCap;
  Opts.StarCap = OrderStarCap;
  Opts.DAGSizeCap = OrderSizeCap;
  Opts.Incremental = !OrderFullRescore;
  Opts.RecordTrace = OrderTrace;
  Opts.MeasureLiveNodes = MeasurePeakOpt;
  Opts.UseSparseElimination = OrderSparse;
  Opts.RandomSeed = OrderSeed;
  if (!OrderExplicit.empty()) {
    if (OrderExplicit.back() == ',') {
      throw std::invalid_argument(
          "APA explicit order must not end with a comma");
    }
    std::stringstream Input(OrderExplicit);
    std::string Token;
    while (std::getline(Input, Token, ',')) {
      unsigned long long Index = 0;
      if (Token.empty() || StringRef(Token).getAsInteger(10, Index)) {
        throw std::invalid_argument(
            "APA explicit order must contain unsigned indices");
      }
      Opts.ExplicitOrder.push_back(static_cast<std::size_t>(Index));
    }
  }
  return Opts;
}

// Build EliminationOptions from the evaluation CLI flags.
elimination::EliminationOptions buildElimOpts() {
  auto Opts = lotus::dataflow_tool::parseEliminationOptions(ElimMethodOpt);
  Opts.Ordering = parseOrderingPolicy();
  Opts.Order = buildOrderOpts();
  Opts.EnableEAN = EanOpt;
  Opts.EnableGreedy = GreedyOpt;
  Opts.EANLaws = (EanLawsOpt == "kleene")
                     ? elimination::ean::LawProfile::kleeneAlgebra()
                     : elimination::ean::LawProfile::safeMinimal();
  Opts.EANCost = (EanCostOpt == "dag") ? elimination::ean::CostModel::dag()
                                       : elimination::ean::CostModel::uniform();
  elimination::ean::Budget B = elimination::ean::Budget::unbounded();
  if (EanRoundLimit)
    B.roundLimit = EanRoundLimit;
  if (EanNodeLimit)
    B.nodeLimit = EanNodeLimit;
  if (EanTimeLimit > 0.0)
    B.timeLimitSec = EanTimeLimit;
  Opts.EANBudget = B;
  Opts.MeasurePeakNodes = MeasurePeakOpt;
  Opts.EANMinNodes = EanMinNodes;
  Opts.EANMonotone = EanMonotoneOpt;
  Opts.InterpRepeat = InterpRepeat ? InterpRepeat : 1;
  Opts.InterpMemo = MemoInterpOpt;
  return Opts;
}

// Build PathSummaryEquationOptions (with the EAN sub-config) for the
// interprocedural path-summary solver from the same evaluation CLI flags.
elimination::PathSummaryEquationOptions buildInterSummaryOpts() {
  elimination::PathSummaryEquationOptions Opts;
  Opts.Ordering = parseOrderingPolicy();
  Opts.Order = buildOrderOpts();
  auto &E = Opts.EAN;
  E.EnableEAN = EanOpt;
  E.EnableGreedy = GreedyOpt;
  E.EANLaws = (EanLawsOpt == "kleene")
                  ? elimination::ean::LawProfile::kleeneAlgebra()
                  : elimination::ean::LawProfile::safeMinimal();
  E.EANCost = (EanCostOpt == "dag") ? elimination::ean::CostModel::dag()
                                    : elimination::ean::CostModel::uniform();
  elimination::ean::Budget B = elimination::ean::Budget::unbounded();
  if (EanRoundLimit)
    B.roundLimit = EanRoundLimit;
  if (EanNodeLimit)
    B.nodeLimit = EanNodeLimit;
  if (EanTimeLimit > 0.0)
    B.timeLimitSec = EanTimeLimit;
  E.EANBudget = B;
  E.EANMinNodes = EanMinNodes;
  E.EANMonotone = EanMonotoneOpt;
  E.InterpRepeat = InterpRepeat ? InterpRepeat : 1;
  return Opts;
}

ValueIdMap buildModuleValueIdMap(Module &M) {
  ValueIdMap ValueToId;
  for (auto &G : M.globals()) {
    ValueToId[&G] = (G.hasName() ? G.getName() : "global").str();
  }

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    const std::string Prefix = F.getName().str();
    unsigned ArgIdx = 0;
    for (auto &Arg : F.args()) {
      ValueToId[&Arg] = Prefix + ".arg" + std::to_string(ArgIdx++);
    }

    unsigned InstIdx = 0;
    for (auto &BB : F) {
      for (auto &I : BB) {
        ValueToId[&I] = Prefix + ".i" + std::to_string(InstIdx++);
      }
    }
  }
  return ValueToId;
}

template <typename ContextT> std::string contextToString(const ContextT &Ctx) {
  std::string Buffer;
  raw_string_ostream OS(Buffer);
  Ctx.print(OS);
  return OS.str();
}

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

const char *toString(elimination::ADTRejectionReason R) {
  using Reason = elimination::ADTRejectionReason;
  switch (R) {
  case Reason::None:
    return "none";
  case Reason::EmptyTopologicalOrder:
    return "empty-topological-order";
  case Reason::DisconnectedFromEntry:
    return "disconnected-from-entry";
  case Reason::NonBackEdgeCycle:
    return "non-back-edge-cycle";
  case Reason::EntryNotFirst:
    return "entry-not-first";
  case Reason::MissingTopologicalNode:
    return "missing-topological-node";
  case Reason::InvalidImmediateDominator:
    return "invalid-immediate-dominator";
  case Reason::ADTConstructionFailed:
    return "adt-construction-failed";
  case Reason::MissingADTLeaf:
    return "missing-adt-leaf";
  case Reason::EdgeClassificationFailed:
    return "edge-classification-failed";
  case Reason::ForwardEdgeMissesIntervalEntry:
    return "forward-edge-misses-interval-entry";
  case Reason::BackEdgeMissesIntervalEntry:
    return "back-edge-misses-interval-entry";
  }
  return "unknown";
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

std::string
formatValueLatticeElement(const elimination::ConstantPropagationValue &Val) {
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

std::string formatSignValue(elimination::SignValue Val) {
  if (Val.isBottom())
    return "bottom";
  std::string Result;
  auto Add = [&](bool Enabled, llvm::StringRef Name) {
    if (!Enabled)
      return;
    if (!Result.empty())
      Result += "|";
    Result += Name.str();
  };
  Add(Val.mayBeNegative(), "neg");
  Add(Val.mayBeZero(), "zero");
  Add(Val.mayBePositive(), "pos");
  return Result;
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

template <typename ExprRefT>
void collectExprProfileImpl(const ExprRefT &Expr, size_t Depth,
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
  using Kind = decltype(Expr->K);
  switch (Expr->K) {
  case Kind::Zero:
    ++Profile.ZeroNodes;
    return;
  case Kind::One:
    ++Profile.OneNodes;
    return;
  case Kind::Atom:
    ++Profile.AtomNodes;
    return;
  case Kind::Union:
    ++Profile.UnionNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case Kind::Concat:
    ++Profile.ConcatNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    collectExprProfileImpl(Expr->R, Depth + 1, Visited, Profile);
    return;
  case Kind::Star:
    ++Profile.StarNodes;
    collectExprProfileImpl(Expr->L, Depth + 1, Visited, Profile);
    return;
  }
}

template <typename ExprRefT>
ExprProfile collectExprProfile(const ExprRefT &Expr) {
  ExprProfile Profile;
  std::unordered_set<const void *> Visited;
  collectExprProfileImpl(Expr, 1, Visited, Profile);
  return Profile;
}

std::string formatTransfer(const Instruction *I, const ValueIdMap &ValueToId) {
  if (!I)
    return "null";
  auto It = ValueToId.find(I);
  return It != ValueToId.end() ? It->second : "inst";
}

std::string formatTransfer(const elimination::NonNullEdgeTransfer &Transfer,
                           const ValueIdMap &ValueToId) {
  return formatTransfer(Transfer.Src, ValueToId) + "->" +
         formatTransfer(Transfer.Dst, ValueToId);
}

std::string formatTransfer(const elimination::AffineEdgeTransfer &Transfer,
                           const ValueIdMap &ValueToId) {
  return formatTransfer(Transfer.inst, ValueToId) + "->" +
         formatTransfer(Transfer.succ, ValueToId);
}

template <typename ExprRefT>
void formatPathExpr(raw_ostream &OS, const ExprRefT &Expr,
                    const ValueIdMap &ValueToId) {
  if (!Expr) {
    OS << "null";
    return;
  }

  using Kind = decltype(Expr->K);
  switch (Expr->K) {
  case Kind::Zero:
    OS << "zero";
    return;
  case Kind::One:
    OS << "one";
    return;
  case Kind::Atom:
    OS << "atom(";
    if (Expr->Transfer)
      OS << formatTransfer(*Expr->Transfer, ValueToId);
    else
      OS << "null";
    OS << ")";
    return;
  case Kind::Union:
    OS << "union(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case Kind::Concat:
    OS << "concat(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ",";
    formatPathExpr(OS, Expr->R, ValueToId);
    OS << ")";
    return;
  case Kind::Star:
    OS << "star(";
    formatPathExpr(OS, Expr->L, ValueToId);
    OS << ")";
    return;
  }
}

void emitOrderingDiagnostics(raw_ostream &OS,
                             const elimination::OrderingDiagnostics &D) {
  if (D.regions == 0) {
    return;
  }
  OS << "  [ordering] regions=" << D.regions << ", pivots=" << D.selected_nodes
     << ", refreshes=" << D.score_refreshes
     << ", stale=" << D.stale_heap_entries
     << ", allocated=" << D.allocated_nodes
     << ", initial_allocated=" << D.initial_allocated_nodes
     << ", elimination_allocated=" << D.elimination_allocated_nodes
     << ", backsubstitution_allocated=" << D.backsubstitution_allocated_nodes
     << ", boundary_allocated=" << D.boundary_allocated_nodes
     << ", star_allocated=" << D.allocated_stars << ", bypasses=" << D.bypasses
     << ", peak_live_nodes=" << D.peak_live_nodes
     << ", peak_live_edges=" << D.peak_live_edges
     << ", peak_active_nodes=" << D.peak_active_nodes
     << ", peak_active_edges=" << D.peak_active_edges
     << ", metadata_ns=" << D.metadata_time_ns
     << ", scoring_ns=" << D.scoring_time_ns << ", heap_ns=" << D.heap_time_ns
     << ", ranking_ns=" << D.metadata_time_ns + D.selection_time_ns << "\n";
  for (const auto &S : D.score_updates) {
    OS << "  [order-score] region=" << S.region << ", step=" << S.step
       << ", node=" << S.node << ", version=" << S.version
       << ", structural=" << S.signals.structural
       << ", expression=" << S.signals.expression << ", star=" << S.signals.star
       << ", score=" << S.score << "\n";
  }
  for (const auto &S : D.trace) {
    OS << "  [order-step] region=" << S.region << ", step=" << S.step
       << ", node=" << S.node << ", structural=" << S.signals.structural
       << ", expression=" << S.signals.expression << ", star=" << S.signals.star
       << ", score=" << S.score << ", allocated=" << S.allocated_nodes
       << ", star_allocated=" << S.allocated_stars
       << ", live_nodes=" << S.live_nodes << ", live_edges=" << S.live_edges
       << ", active_nodes=" << S.active_nodes
       << ", active_edges=" << S.active_edges << ", bypasses=" << S.bypasses
       << "\n";
  }
}

template <typename ResultT> void recordSolveStatus(const ResultT &Result) {
  if (Result.hasSolveMetadata()) {
    HadSolveError |=
        Result.solveStatus() == elimination::SolveStatus::InvalidProblem ||
        Result.solveStatus() == elimination::SolveStatus::NonConvergentStar;
  }
}

template <typename ResultT>
void printSolveMetadata(raw_ostream &OS, const ResultT &Result) {
  if (!Result.hasSolveMetadata())
    return;
  const auto &Diag = Result.solveDiagnostics();
  recordSolveStatus(Result);
  OS << "  [solver] status=" << toString(Result.solveStatus())
     << ", requested=" << toString(Diag.requested_method)
     << ", executed=" << toString(Diag.executed_method)
     << ", used_adt=" << (Diag.used_adt ? "true" : "false")
     << ", fallback=" << toString(Diag.fallback_reason)
     << ", adt_reason=" << toString(Diag.adt_rejection_reason)
     << ", star_iters=" << Diag.star_iterations_total
     << ", max_star_hit=" << (Diag.max_star_hit ? "true" : "false")
     << ", ean_laws_restricted="
     << (Diag.ean_laws_restricted ? "true" : "false")
     << ", peak_nodes=" << Diag.peak_matrix_nodes
     << ", semantic_star_ns=" << Diag.semantic_star_time_ns << "\n";
  emitOrderingDiagnostics(OS, Diag.ordering);
}

template <unsigned K, typename FactT, typename TransferT, typename NodeT>
void printSolveMetadata(
    raw_ostream &OS,
    const elimination::InterDataFlowResultT<K, FactT, TransferT, NodeT>
        &Result) {
  if (!Result.hasSolveMetadata())
    return;
  recordSolveStatus(Result);
  OS << "  [solver] status=" << toString(Result.solveStatus()) << "\n";
  if (Result.hasContextSolveDiagnostics()) {
    const auto &D = Result.contextSolveDiagnostics();
    OS << "  [context-summary] pairs=" << D.procedure_context_count
       << ", builds=" << D.procedure_summary_builds
       << ", reuses=" << D.procedure_summary_reuses
       << ", gen_us=" << D.gen_time_us << ", norm_us=" << D.norm_time_us
       << ", interp_us=" << D.interp_time_us
       << ", semantic_star_ns=" << D.semantic_star_time_ns
       << ", star_iters=" << D.star_iterations_total << "\n";
    emitOrderingDiagnostics(OS, D.ordering);
  }
}

template <typename ResultT>
void dumpProfile(raw_ostream &OS, const FunctionView &View,
                 const ResultT &Result, const Timings &T) {
  const auto CFG = collectCFGStats(View.Function);
  OS << "  [cfg] args=" << CFG.Arguments << ", blocks=" << CFG.Blocks
     << ", insts=" << CFG.Instructions << ", edges=" << CFG.Edges
     << ", branching_blocks=" << CFG.BranchingBlocks
     << ", max_succs=" << CFG.MaxSuccessors << ", phis=" << CFG.PhiNodes
     << ", calls=" << CFG.Calls << ", returns=" << CFG.Returns
     << ", unreachable=" << CFG.Unreachable << ", elapsed_us=" << T.end2end
     << "\n";
  OS << "  [timing] gen_us=" << T.gen << ", norm_us=" << T.norm
     << ", interp_us=" << T.interp << ", end2end_us=" << T.end2end
     << ", runs=" << T.runs << "\n";
  printSolveMetadata(OS, Result);

  // Unified DAG statistics over the whole summary batch (matches the synthetic
  // evaluation's DagStats — the Table VI structural metrics). The transfer type
  // is deduced from the result so clients with a non-Instruction* atom (e.g.
  // NonNull's edge transfer) profile correctly.
  using ResultTransferT = typename ResultT::transfer_t;
  using BatchExprRef =
      typename elimination::PathExprFactory<ResultTransferT>::Ref;
  std::vector<BatchExprRef> Batch;
  Batch.reserve(View.OrderedInsts.size());
  for (auto *I : View.OrderedInsts) {
    auto E = Result.ExprTo(I);
    if (E)
      Batch.push_back(E);
  }
  const auto DS = elimination::ean::computeDagStats<ResultTransferT>(Batch);
  OS << "  [dagstats] nodes=" << DS.uniqueNodes << ", edges=" << DS.uniqueEdges
     << ", tree=" << DS.expandedTree << ", seq=" << DS.concats
     << ", stars=" << DS.stars << ", unions=" << DS.unions
     << ", atoms=" << DS.atoms << ", sharing=" << DS.sharing()
     << ", roots=" << Batch.size() << "\n";

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

  for (auto *I : View.OrderedInsts) {
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
      MaxExprInst = View.ValueToId.at(I);
    }
    if (Profile.MaxDepth > MaxExprDepth) {
      MaxExprDepth = Profile.MaxDepth;
      DeepestExprInst = View.ValueToId.at(I);
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

  for (auto *I : View.OrderedInsts) {
    const auto Expr = Result.ExprTo(I);
    OS << "  [expr] " << View.ValueToId.at(I);
    if (!Expr) {
      OS << " missing\n";
      continue;
    }
    const auto Profile = collectExprProfile(Expr);
    OS << " nodes=" << Profile.UniqueNodes << ", depth=" << Profile.MaxDepth
       << ", atoms=" << Profile.AtomNodes << ", unions=" << Profile.UnionNodes
       << ", concats=" << Profile.ConcatNodes << ", stars=" << Profile.StarNodes
       << ", shared_refs=" << Profile.SharedRefs << ", expr=";
    formatPathExpr(OS, Expr, View.ValueToId);
    OS << "\n";
  }
}

template <typename ResultT, typename Printer>
void dumpTimedResult(raw_ostream &OS, const FunctionView &View, ResultT &Result,
                     const Timings &T, Printer &&PrintState) {
  const bool HasOutput = StdoutOpt || !OutDir.empty();
  if (HasOutput && (DumpProfileOpt || DumpExprsOpt))
    dumpProfile(OS, View, Result, T);
  if (ProfileOnlyOpt || !HasOutput)
    return;
  lotus::dataflow_tool::printInstructionStates(
      OS, View, [&](Instruction *I) { PrintState(I, Result); });
}

template <typename Runner, typename Printer>
void runTimedAnalysis(raw_ostream &OS, const FunctionView &View,
                      const elimination::EliminationOptions &ElimOpts,
                      Runner &&Run, Printer &&PrintState) {
  for (unsigned W = 0; W < WarmupOpt; ++W) {
    auto Warm = Run(View.Function, ElimOpts);
    (void)Warm;
  }
  const unsigned R = std::max(1u, static_cast<unsigned>(RepeatOpt));
  std::vector<std::uint64_t> Gen, Norm, Interp, End;
  Gen.reserve(R);
  Norm.reserve(R);
  Interp.reserve(R);
  End.reserve(R);

  // The R-1 timing-only runs are constructed and discarded (DataFlowResultT is
  // not assignable, so we never reassign — we construct fresh each run).
  auto Sample = [&](const auto &Res, std::uint64_t Us) {
    const auto &D = Res.solveDiagnostics();
    Gen.push_back(D.gen_time_us);
    Norm.push_back(D.norm_time_us);
    Interp.push_back(D.interp_time_us);
    End.push_back(Us);
  };
  for (unsigned I = 0; I + 1 < R; ++I) {
    const auto Start = std::chrono::steady_clock::now();
    auto Tmp = Run(View.Function, ElimOpts);
    Sample(Tmp, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - Start)
                        .count()));
  }
  // Final measured run is kept for the structural dump.
  const auto Start = std::chrono::steady_clock::now();
  auto Result = Run(View.Function, ElimOpts);
  recordSolveStatus(Result);
  Sample(Result, static_cast<std::uint64_t>(
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - Start)
                         .count()));

  const Timings T{medianOf(Gen), medianOf(Norm), medianOf(Interp),
                  medianOf(End), R};
  dumpTimedResult(OS, View, Result, T, std::forward<Printer>(PrintState));
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
void runTimedInterproceduralAnalysis(raw_ostream &OS, Module &M,
                                     Function &Entry, Runner &&Run,
                                     Printer &&PrintState) {
  const auto Start = std::chrono::steady_clock::now();
  auto Result = Run(Entry);
  recordSolveStatus(Result);
  const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - Start);
  const bool HasOutput = StdoutOpt || !OutDir.empty();
  if (HasOutput) {
    OS << "  [profile] elapsed_us=" << Elapsed.count() << "\n";
    printSolveMetadata(OS, Result);
  }
  if (ProfileOnlyOpt || !HasOutput)
    return;
  const auto ValueToId = buildModuleValueIdMap(M);
  dumpInterproceduralResult(OS, M, ValueToId, Result,
                            [&](const auto &Key, const auto &Res) {
                              PrintState(Key, Res, ValueToId);
                            });
}

// Emit the path-summary solver's Table VI/VII diagnostics (equation graph, the
// EAN/Greedy stage timings, and the batch structural stats before/after
// optimization). Within one EAN/Greedy run, dagstats-before is the raw
// (Default) batch and dagstats-after is the optimized one.
template <unsigned K, typename FactT, typename TransferT, typename NodeT>
void emitInterSummaryDiagnostics(
    raw_ostream &OS,
    const elimination::InterDataFlowResultT<K, FactT, TransferT, NodeT>
        &Result) {
  if (!Result.hasSummarySolveDiagnostics())
    return;
  const auto &D = Result.summarySolveDiagnostics();
  OS << "  [inter-summary] contexts=" << D.discovered_context_node_count
     << ", seeds=" << D.seed_count << ", eqn_nodes=" << D.equation_node_count
     << ", eqn_edges=" << D.equation_edge_count << ", scc=" << D.scc_count
     << ", cyclic_scc=" << D.cyclic_scc_count << ", gen_us=" << D.gen_time_us
     << ", norm_us=" << D.norm_time_us << ", interp_us=" << D.interp_time_us
     << ", semantic_star_ns=" << D.semantic_star_time_ns
     << ", star_iters=" << D.star_iterations_total << "\n";
  emitOrderingDiagnostics(OS, D.ordering);
  auto EmitStats = [&](const char *Tag, const elimination::ean::DagStats &S) {
    OS << "  [" << Tag << "] nodes=" << S.uniqueNodes
       << ", edges=" << S.uniqueEdges << ", tree=" << S.expandedTree
       << ", seq=" << S.concats << ", stars=" << S.stars
       << ", unions=" << S.unions << ", atoms=" << S.atoms
       << ", sharing=" << S.sharing() << "\n";
  };
  EmitStats("dagstats-before", D.summary_before);
  EmitStats("dagstats-after", D.summary_after);
}

// Run one interprocedural client through the path-summary solver (EAN/Greedy
// applied to the summary batch) and emit timing + Table VI/VII diagnostics,
// then the per-(inst,ctx) facts (for RQ1-inter differential).
template <typename Runner, typename Printer>
void runInterSummaryAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                             Runner &&Run, Printer &&PrintState) {
  const auto Start = std::chrono::steady_clock::now();
  auto Result = Run(Entry);
  const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - Start);
  const auto ValueToId = buildModuleValueIdMap(M);
  OS << "  [profile] elapsed_us=" << Elapsed.count() << "\n";
  printSolveMetadata(OS, Result);
  emitInterSummaryDiagnostics(OS, Result);
  dumpInterproceduralResult(OS, M, ValueToId, Result,
                            [&](const auto &Key, const auto &Res) {
                              PrintState(Key, Res, ValueToId);
                            });
}

// Summary-solver variants of the interprocedural clients. reachable is clean
// (Entry, ICF, Options); the others take LLVM analyses that we leave null (the
// summary problem tolerates null AA/MSSA/AC/DT/TLI, degrading precision but not
// crashing) — the driver records any client that fails to run.
void runInterSummaryReachable(raw_ostream &OS, Module &M, Function &Entry) {
  auto Opts = buildInterSummaryOpts();
  runInterSummaryAnalysis(
      OS, M, Entry,
      [&](Function &F) {
        return elimination::runInterSummaryElimReachability(&F, nullptr, Opts);
      },
      [&](const auto &Key, const auto &Result, const auto &) {
        OS << (Result.IN(Key) ? "true" : "false");
      });
}

void runInterSummaryReachingDefinitions(raw_ostream &OS, Module &M,
                                        Function &Entry) {
  auto Opts = buildInterSummaryOpts();
  runInterSummaryAnalysis(
      OS, M, Entry,
      [&](Function &F) {
        return elimination::runInterSummaryElimReachingDefinitions(
            &F, nullptr, nullptr, nullptr, Opts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

void runInterSummaryUninitialized(raw_ostream &OS, Module &M, Function &Entry) {
  auto Opts = buildInterSummaryOpts();
  runInterSummaryAnalysis(
      OS, M, Entry,
      [&](Function &F) {
        return elimination::runInterSummaryElimUninitializedVariables(
            &F, nullptr, nullptr, nullptr, nullptr, Opts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

void runInterSummaryConstantPropagation(raw_ostream &OS, Module &M,
                                        Function &Entry) {
  auto Opts = buildInterSummaryOpts();
  runInterSummaryAnalysis(
      OS, M, Entry,
      [&](Function &F) {
        return elimination::runInterSummaryElimConstantPropagation(
            &F, nullptr, nullptr, nullptr, nullptr, nullptr, Opts);
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
                                         Function &Entry) {
  auto Opts = buildInterSummaryOpts();
  runInterSummaryAnalysis(
      OS, M, Entry,
      [&](Function &F) {
        return elimination::runInterSummaryElimAvailableExpressions(&F, nullptr,
                                                                    Opts);
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

void runInterSummaryLockset(raw_ostream &OS, Module &M, Function &Entry) {
  auto Opts = buildInterSummaryOpts();
  runInterSummaryAnalysis(
      OS, M, Entry,
      [&](Function &F) {
        return elimination::runInterSummaryElimLockset(&F, nullptr, Opts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

void runInterSummaryNonNull(raw_ostream &OS, Module &M, Function &Entry) {
  auto Opts = buildInterSummaryOpts();
  runInterSummaryAnalysis(
      OS, M, Entry,
      [&](Function &F) {
        return elimination::runInterSummaryElimNonNull(&F, nullptr, nullptr,
                                                       nullptr, Opts);
      },
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

void runInterSummarySign(raw_ostream &OS, Module &M, Function &Entry) {
  auto Opts = buildInterSummaryOpts();
  runInterSummaryAnalysis(
      OS, M, Entry,
      [&](Function &F) {
        return elimination::runInterSummaryElimSign(&F, nullptr, Opts);
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
                         Runner &&Run) {
  runTimedAnalysis(OS, View, ElimOpts, std::forward<Runner>(Run),
                   [&](Instruction *I, auto &Result) {
                     lotus::dataflow_tool::formatValueSet(OS, Result.IN(I),
                                                          View.ValueToId);
                   });
}

template <typename Runner>
void runBoolIntraAnalysis(raw_ostream &OS, const FunctionView &View,
                          const elimination::EliminationOptions &ElimOpts,
                          Runner &&Run) {
  runTimedAnalysis(OS, View, ElimOpts, std::forward<Runner>(Run),
                   [&](Instruction *I, auto &Result) {
                     OS << (Result.IN(I) ? "true" : "false");
                   });
}

template <typename Runner>
void runSetInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                         Runner &&Run) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueSet(OS, Result.IN(Key), ValueToId);
      });
}

template <typename Runner, typename Formatter>
void runMapInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                         Runner &&Run, Formatter &&FormatValue) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto &ValueToId) {
        lotus::dataflow_tool::formatValueMap(OS, Result.IN(Key), ValueToId,
                                             FormatValue);
      });
}

template <typename Runner>
void runBoolInterAnalysis(raw_ostream &OS, Module &M, Function &Entry,
                          Runner &&Run) {
  runTimedInterproceduralAnalysis(
      OS, M, Entry, std::forward<Runner>(Run),
      [&](const auto &Key, const auto &Result, const auto & /*ValueToId*/) {
        OS << (Result.IN(Key) ? "true" : "false");
      });
}

void runReachingDefinitions(raw_ostream &OS, const FunctionView &View,
                            const elimination::EliminationOptions &ElimOpts) {
  const bool TranslApa = (InterpOpt == "translapa");
  OS << "  [interp] mode=" << (TranslApa ? "translapa" : "generic") << "\n";
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [TranslApa](Function &F, const elimination::EliminationOptions &Opts) {
        return TranslApa ? elimination::runIntraTranslApaReachingDefinitions(
                               &F, nullptr, Opts)
                         : elimination::runIntraElimReachingDefinitions(
                               &F, nullptr, Opts);
      });
}

void runUninitialized(raw_ostream &OS, const FunctionView &View,
                      const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimUninitializedVariables(&F, nullptr,
                                                               Opts);
      });
}

// Canonical, vocabulary-free serialization of an affine relation: the Howell
// normal form's rows are already canonical, so equal relations serialize
// identically. This lets the RQ1 differential compare Default vs
// EAN(safe/kleene) affine facts as text WITHOUT needing the (already-freed)
// per-function vocabulary at print time.
std::string serializeAffine(const elimination::AffineRelation &R) {
  if (R.bottom)
    return "bottom";
  std::string Buf;
  raw_string_ostream OS(Buf);
  bool FirstComp = true;
  for (const auto &Entry : R.components) {
    if (!FirstComp)
      OS << "|";
    FirstComp = false;
    OS << "w" << Entry.first << ":";
    std::vector<std::string> Rows;
    Rows.reserve(Entry.second.constraints.size());
    for (const auto &Row : Entry.second.constraints) {
      std::string RowStr;
      for (std::size_t i = 0; i < Row.size(); ++i) {
        if (i)
          RowStr += ",";
        RowStr += std::to_string(Row[i].getZExtValue());
      }
      Rows.push_back(std::move(RowStr));
    }
    std::sort(Rows.begin(),
              Rows.end()); // guard against row-order nondeterminism
    bool FirstRow = true;
    for (auto &Row : Rows) {
      if (!FirstRow)
        OS << ";";
      FirstRow = false;
      OS << Row;
    }
  }
  return OS.str();
}

void runAffine(raw_ostream &OS, const FunctionView &View,
               const elimination::EliminationOptions &ElimOpts) {
  runTimedAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimAffineEqualities(&F, Opts);
      },
      [&](Instruction *I, auto &Result) {
        OS << serializeAffine(Result.IN(I));
      });
}

void runConstantPropagation(raw_ostream &OS, const FunctionView &View,
                            const elimination::EliminationOptions &ElimOpts) {
  runTimedAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimConstantPropagation(&F, nullptr, Opts);
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
                             const elimination::EliminationOptions &ElimOpts) {
  runTimedAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimAvailableExpressions(&F, nullptr, Opts);
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
                const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimLockset(&F, Opts);
      });
}

void runNonNull(raw_ostream &OS, const FunctionView &View,
                const elimination::EliminationOptions &ElimOpts) {
  runSetIntraAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimNonNull(&F, Opts);
      });
}

void runSign(raw_ostream &OS, const FunctionView &View,
             const elimination::EliminationOptions &ElimOpts) {
  runTimedAnalysis(
      OS, View, ElimOpts,
      [](Function &F, const elimination::EliminationOptions &Opts) {
        return elimination::runIntraElimSign(&F, Opts);
      },
      [&](Instruction *I, auto &Result) {
        lotus::dataflow_tool::formatValueMap(OS, Result.IN(I), View.ValueToId,
                                             [](elimination::SignValue Value) {
                                               return formatSignValue(Value);
                                             });
      });
}

void runReachable(raw_ostream &OS, const FunctionView &View,
                  const elimination::EliminationOptions &ElimOpts) {
  const bool TranslApa = (InterpOpt == "translapa");
  OS << "  [interp] mode=" << (TranslApa ? "translapa" : "generic") << "\n";
  runBoolIntraAnalysis(
      OS, View, ElimOpts,
      [TranslApa](Function &F, const elimination::EliminationOptions &Opts) {
        return TranslApa ? elimination::runIntraTranslApaReachability(&F, Opts)
                         : elimination::runIntraElimReachability(&F, Opts);
      });
}

void runInterReachingDefinitions(raw_ostream &OS, Module &M, Function &Entry) {
  if (InterEngineOpt == "expanded") {
    runInterSummaryReachingDefinitions(OS, M, Entry);
    return;
  }
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimReachingDefinitions(
        &F, nullptr, nullptr, nullptr, buildElimOpts());
  });
}

void runInterUninitialized(raw_ostream &OS, Module &M, Function &Entry) {
  if (InterEngineOpt == "expanded") {
    runInterSummaryUninitialized(OS, M, Entry);
    return;
  }
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimUninitializedVariables(
        &F, nullptr, nullptr, nullptr, nullptr, buildElimOpts());
  });
}

void runInterConstantPropagation(raw_ostream &OS, Module &M, Function &Entry) {
  if (InterEngineOpt == "expanded") {
    runInterSummaryConstantPropagation(OS, M, Entry);
    return;
  }
  runMapInterAnalysis(
      OS, M, Entry,
      [](Function &F) {
        return elimination::runInterElimConstantPropagation(
            &F, nullptr, nullptr, nullptr, nullptr, nullptr, buildElimOpts());
      },
      [&](const elimination::ConstantPropagationValue &Value) {
        return formatValueLatticeElement(Value);
      });
}

void runInterAvailableExpressions(raw_ostream &OS, Module &M, Function &Entry) {
  if (InterEngineOpt == "expanded") {
    runInterSummaryAvailableExpressions(OS, M, Entry);
    return;
  }
  runTimedInterproceduralAnalysis(
      OS, M, Entry,
      [](Function &F) {
        return elimination::runInterElimAvailableExpressions(&F, nullptr,
                                                             buildElimOpts());
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

void runInterLockset(raw_ostream &OS, Module &M, Function &Entry) {
  if (InterEngineOpt == "expanded") {
    runInterSummaryLockset(OS, M, Entry);
    return;
  }
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimLockset(&F, nullptr, buildElimOpts());
  });
}

void runInterNonNull(raw_ostream &OS, Module &M, Function &Entry) {
  if (InterEngineOpt == "expanded") {
    runInterSummaryNonNull(OS, M, Entry);
    return;
  }
  runSetInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimNonNull(&F, nullptr, nullptr, nullptr,
                                            buildElimOpts());
  });
}

void runInterSign(raw_ostream &OS, Module &M, Function &Entry) {
  if (InterEngineOpt == "expanded") {
    runInterSummarySign(OS, M, Entry);
    return;
  }
  runMapInterAnalysis(
      OS, M, Entry,
      [](Function &F) {
        return elimination::runInterElimSign(&F, nullptr, buildElimOpts());
      },
      [](elimination::SignValue Value) { return formatSignValue(Value); });
}

void runInterReachable(raw_ostream &OS, Module &M, Function &Entry) {
  if (InterEngineOpt == "modular") {
    auto Opts = buildInterSummaryOpts();
    runInterSummaryAnalysis(
        OS, M, Entry,
        [&](Function &F) {
          return elimination::runModularInterReachability(&F, nullptr, Opts);
        },
        [&](const auto &Key, const auto &Result, const auto &) {
          OS << (Result.IN(Key) ? "true" : "false");
        });
    return;
  }
  if (InterEngineOpt == "expanded") {
    runInterSummaryReachable(OS, M, Entry);
    return;
  }
  runBoolInterAnalysis(OS, M, Entry, [](Function &F) {
    return elimination::runInterElimReachability(&F, nullptr, buildElimOpts());
  });
}

void runInterAffine(raw_ostream &OS, Module &M, Function & /*Entry*/) {
  const auto Start = std::chrono::steady_clock::now();
  elimination::InterAffineEqualitiesOptions Options;
  Options.vocabulary = elimination::InterAffineVocabularyMode::ObservableSlice;
  Options.verbose = false;
  Options.maxTrackedValues = AffineMaxTrackedOpt;
  Options.ordering = parseOrderingPolicy();
  Options.order = buildOrderOpts();
  auto Result = elimination::runInterElimAffineEqualities(M, Options);
  HadSolveError |= Result.status == elimination::SolveStatus::InvalidProblem ||
                   Result.status == elimination::SolveStatus::NonConvergentStar;
  emitOrderingDiagnostics(OS, Result.diagnostics.ordering);
  OS << "  [semantic-star] time_ns=" << Result.diagnostics.semantic_star_time_ns
     << ", iterations=" << Result.diagnostics.star_iterations_total << "\n";
  const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - Start);
  OS << "  [profile] elapsed_us=" << Elapsed.count()
     << ", tracked_values=" << Result.trackedValues
     << ", summaries=" << Result.summaries.size()
     << ", block_relations=" << Result.blockRelations.size()
     << ", status=" << toString(Result.status) << "\n";
}

struct AnalysisHandler final {
  StringRef Name;
  bool ModuleScoped = false;
  void (*RunFunction)(raw_ostream &, const FunctionView &,
                      const elimination::EliminationOptions &) = nullptr;
  void (*RunModule)(raw_ostream &, Module &, Function &) = nullptr;
};

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

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Elimination engine testing\n");
  try {
    const auto Policy = parseOrderingPolicy();
    elimination::order::validateOrderOptions(
        buildOrderOpts(), elimination::OrderingPolicy::Default, 0);
    if (InterEngineOpt != "context" && InterEngineOpt != "expanded" &&
        InterEngineOpt != "modular") {
      throw std::invalid_argument("unknown interprocedural engine: " +
                                  InterEngineOpt);
    }
    if (InterEngineOpt == "modular" && AnalysisOpt != "inter_reachable") {
      throw std::invalid_argument(
          "the modular CLI currently supports inter_reachable only");
    }
    if (OrderTrace) {
      DumpProfileOpt = true;
    }
  } catch (const std::invalid_argument &Error) {
    errs() << "error: " << Error.what() << "\n";
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
      StdoutOpt, OutDir, "elim.txt", FileOS, NullOS, EC);
  if (EC) {
    errs() << "error: cannot create " << OutDir << "/elim.txt: " << EC.message()
           << "\n";
    return 1;
  }

  // Parse a comma-separated client list (amortizes module loading across
  // clients in one process).
  std::vector<const AnalysisHandler *> Clients;
  {
    std::stringstream SS(AnalysisOpt);
    std::string Name;
    while (std::getline(SS, Name, ',')) {
      if (Name.empty())
        continue;
      const auto *H =
          lotus::dataflow_tool::findHandler(StringRef(Name), Handlers);
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

  const auto ElimOpts = buildElimOpts();
  OS << "[elim] clients=" << AnalysisOpt << ", method=" << ElimMethodOpt
     << ", ordering=" << OrderingOpt << ", ean=" << (EanOpt ? "on" : "off")
     << ", ean_laws=" << EanLawsOpt << ", repeat=" << RepeatOpt
     << ", ean_min_nodes=" << EanMinNodes << ", interp_repeat=" << InterpRepeat
     << ", max_func_insts=" << MaxFuncInsts << "\n";

  const bool AnyModule =
      std::any_of(Clients.begin(), Clients.end(),
                  [](const AnalysisHandler *H) { return H->ModuleScoped; });
  if (AnyModule) {
    if (Clients.size() != 1) {
      errs() << "error: module-scoped analyses must be run one at a time\n";
      return 1;
    }
    Function *Entry = M->getFunction(EntryFunctionOpt);
    if (Entry == nullptr || Entry->isDeclaration()) {
      errs() << "error: entry function '" << EntryFunctionOpt
             << "' not found or is a declaration\n";
      return 1;
    }
    Clients.front()->RunModule(OS, *M, *Entry);
  } else {
    std::size_t Skipped = 0;
    lotus::dataflow_tool::forEachDefinedFunction(
        *M, OS, [&](const FunctionView &View) {
          if (MaxFuncInsts != 0 && View.OrderedInsts.size() > MaxFuncInsts) {
            OS << "  [skipped] reason=too_large insts="
               << View.OrderedInsts.size() << "\n";
            ++Skipped;
            return;
          }
          for (const AnalysisHandler *H : Clients) {
            OS << "  [client:" << H->Name << "]\n";
            H->RunFunction(OS, View, ElimOpts);
          }
        });
    OS << "[summary] skipped_functions=" << Skipped << "\n";
  }

  OS << "[mem] peak_rss_kb=" << peakRssKb() << "\n";
  return HadSolveError ? 1 : 0;
}
