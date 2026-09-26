#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Tooling/APADriver.h"
#include "Dataflow/Tooling/ToolSupport.h"

#include <memory>
#include <string>
#include <system_error>

using namespace llvm;

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

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Elimination engine testing\n");

  elimination::tooling::APADriverOptions Opts;
  Opts.OutDir = OutDir;
  Opts.Stdout = StdoutOpt;
  Opts.Analysis = AnalysisOpt;
  Opts.EntryFunction = EntryFunctionOpt;
  Opts.ElimMethod = ElimMethodOpt;
  Opts.DumpProfile = DumpProfileOpt;
  Opts.ProfileOnly = ProfileOnlyOpt;
  Opts.DumpExprs = DumpExprsOpt;
  Opts.AffineMaxTracked = AffineMaxTrackedOpt;
  Opts.Ordering = OrderingOpt;
  Opts.OrderStructCap = OrderStructCap;
  Opts.OrderExprCap = OrderExprCap;
  Opts.OrderStarCap = OrderStarCap;
  Opts.OrderSizeCap = OrderSizeCap;
  Opts.OrderFullRescore = OrderFullRescore;
  Opts.OrderTrace = OrderTrace;
  Opts.OrderSparse = OrderSparse;
  Opts.OrderSeed = OrderSeed;
  Opts.OrderExplicit = OrderExplicit;
  Opts.Ean = EanOpt;
  Opts.Greedy = GreedyOpt;
  Opts.EanMonotone = EanMonotoneOpt;
  Opts.EanLaws = EanLawsOpt;
  Opts.EanCost = EanCostOpt;
  Opts.EanRoundLimit = EanRoundLimit;
  Opts.EanNodeLimit = EanNodeLimit;
  Opts.EanTimeLimit = EanTimeLimit;
  Opts.MeasurePeak = MeasurePeakOpt;
  Opts.MaxFuncInsts = MaxFuncInsts;
  Opts.EanMinNodes = EanMinNodes;
  Opts.InterpRepeat = InterpRepeat;
  Opts.InterEngine = InterEngineOpt;
  Opts.MemoInterp = MemoInterpOpt;
  Opts.Interp = InterpOpt;

  try {
    elimination::tooling::validateDriverOptions(Opts);
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
      Opts.Stdout, Opts.OutDir, "elim.txt", FileOS, NullOS, EC);
  if (EC) {
    errs() << "error: cannot create " << Opts.OutDir << "/elim.txt: "
           << EC.message() << "\n";
    return 1;
  }

  return elimination::tooling::runApaDriver(*M, OS, Opts);
}
