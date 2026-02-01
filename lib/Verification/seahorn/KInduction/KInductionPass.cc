// K-Induction engine: reuses Seahorn PathBMC.
// For each k, peels loops (k+1) times and runs PathBMC; SAFE if UNSAT, BUG if SAT.

#include "seahorn/config.h"

#ifndef HAVE_CLAM
// PathBMC with CLAM is required for full k-induction.
#include "seahorn/KInduction.hh"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

namespace seahorn {

llvm::Pass *createKInductionPass(llvm::raw_ostream *out) {
  return new KInductionPass(out);
}

char KInductionPass::ID = 0;

KInductionPass::KInductionPass(llvm::raw_ostream *out) : llvm::ModulePass(ID), m_out(out) {}

void KInductionPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool KInductionPass::runOnModule(llvm::Module &M) {
  if (m_out)
    *m_out << "k-induction: PathBMC engine requires CLAM (HAVE_CLAM). Result: unknown\n";
  return false;
}

} // namespace seahorn
#else

#include "seahorn/KInduction.hh"
#include "seahorn/PathBmc.hh"
#include "seahorn/Passes.hh"
#include "seahorn/BvOpSem.hh"
#include "seahorn/Support/SeaDebug.h"
#include "seahorn/Support/SeaLog.hh"
#include "seahorn/Support/Stats.hh"
#include "seahorn/Analysis/CanFail.hh"
#include "seahorn/Analysis/CutPointGraph.hh"
#include "seahorn/Analysis/SeaBuiltinsInfo.hh"
#include "seahorn/Analysis/TopologicalOrder.hh"
#include "seahorn/Transforms/Utils/NameValues.hh"
#include "seahorn/InitializePasses.hh"

#include "Alias/seadsa/ShadowMem.hh"

#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

#include <chrono>
#include <memory>

using namespace llvm;
using namespace seahorn;

static cl::opt<unsigned> KInductionKMin(
    "kinduction-k-min",
    cl::desc("K-Induction: minimum k to try"),
    cl::init(1));

static cl::opt<unsigned> KInductionKMax(
    "kinduction-k-max",
    cl::desc("K-Induction: maximum k (0 = no limit)"),
    cl::init(0));

static cl::opt<unsigned> KInductionTimeout(
    "kinduction-timeout",
    cl::desc("K-Induction: total timeout in seconds (0 = no timeout)"),
    cl::init(0));

// Pass that runs PathBMC and stores the result for KInduction to read.
namespace {
class PathBmcRunnerPass : public ModulePass {
public:
  static char ID;
  static solver::SolverResult s_lastResult;

  PathBmcRunnerPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    Function *main = M.getFunction("main");
    if (!main || main->isDeclaration()) {
      s_lastResult = solver::SolverResult::UNKNOWN;
      return false;
    }
    return runOnFunction(*main, M);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<seadsa::ShadowMemPass>();
    AU.addRequired<LazyValueInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<CanFail>();
    AU.addRequired<NameValues>();
    AU.addRequired<TopologicalOrder>();
    AU.addRequired<CutPointGraph>();
    AU.setPreservesAll();
  }

  static solver::SolverResult getLastResult() { return s_lastResult; }

private:
  bool runOnFunction(Function &F, Module &M) {
    const CutPointGraph &cpg = getAnalysis<CutPointGraph>(F);
    const CutPoint &src = cpg.getCp(F.getEntryBlock());
    const CutPoint *dst = nullptr;

    for (auto &bb : F)
      if (isa<ReturnInst>(bb.getTerminator()) && cpg.isCutPoint(bb)) {
        dst = &cpg.getCp(bb);
        break;
      }

    if (!dst) {
      WARN << "K-Induction: no unique return block in " << F.getName();
      s_lastResult = solver::SolverResult::UNKNOWN;
      return false;
    }

    if (!cpg.getEdge(src, *dst)) {
      WARN << "K-Induction: no direct entry-to-exit path (loops?).";
      s_lastResult = solver::SolverResult::UNKNOWN;
      return false;
    }

    ExprFactory efac;
    const auto &dl = M.getDataLayout();
    auto sem = std::make_unique<BvOpSem>(efac, *this, dl, MEM);
    auto &sm = getAnalysis<seadsa::ShadowMemPass>().getShadowMem();
    auto &tli = getAnalysis<TargetLibraryInfoWrapperPass>();

    PathBmcEngine bmc(*sem, tli, sm);
    bmc.addCutPoint(src);
    bmc.addCutPoint(*dst);

    s_lastResult = bmc.solve();
    return false;
  }
};

char PathBmcRunnerPass::ID = 0;
solver::SolverResult PathBmcRunnerPass::s_lastResult = solver::SolverResult::UNKNOWN;
} // namespace

static llvm::RegisterPass<PathBmcRunnerPass> XPathBmcRunner("path-bmc-runner",
  "Run PathBMC and store result (internal for K-Induction)");
static llvm::RegisterPass<KInductionPass> XKInduction("kinduction",
  "K-Induction verification (PathBMC-based)");

// ---------------------------------------------------------------------------
// KInductionPass
// ---------------------------------------------------------------------------

namespace seahorn {

llvm::Pass *createKInductionPass(llvm::raw_ostream *out) {
  return new KInductionPass(out);
}

char KInductionPass::ID = 0;

KInductionPass::KInductionPass(llvm::raw_ostream *out)
    : llvm::ModulePass(ID), m_out(out) {}

void KInductionPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<seadsa::ShadowMemPass>();
  AU.addRequired<LazyValueInfoWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<CanFail>();
  AU.addRequired<NameValues>();
  AU.addRequired<TopologicalOrder>();
  AU.addRequired<CutPointGraph>();
  AU.addRequired<SeaBuiltinsInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequiredID(llvm::LoopSimplifyID);
  AU.addRequiredID(llvm::LCSSAID);
  AU.setPreservesAll();
}

bool KInductionPass::runOnModule(llvm::Module &M) {
  Function *main = M.getFunction("main");
  if (!main || main->isDeclaration()) {
    if (m_out)
      *m_out << "k-induction: no main(); result: unknown\n";
    return false;
  }

  unsigned k_min = KInductionKMin;
  unsigned k_max = KInductionKMax;
  unsigned timeout_sec = KInductionTimeout;
  auto start = std::chrono::steady_clock::now();

  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeLoopPeelerPassPass(Registry);

  // Inner pass manager: peel once, re-run analyses, then PathBMC.
  llvm::legacy::PassManager innerPM;
  innerPM.add(new llvm::DominatorTreeWrapperPass());
  innerPM.add(new llvm::LoopInfoWrapperPass());
  innerPM.add(new llvm::ScalarEvolutionWrapperPass());
  innerPM.add(new SeaBuiltinsInfoWrapperPass());
  innerPM.add(llvm::createLoopSimplifyPass());
  innerPM.add(llvm::createLCSSAPass());
  innerPM.add(seahorn::createLoopPeelerPass(1));
  innerPM.add(new TopologicalOrder());
  innerPM.add(new CutPointGraph());
  innerPM.add(new PathBmcRunnerPass());

  for (unsigned k = 1;; ++k) {
    if (timeout_sec > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start).count();
      if (elapsed >= (long)timeout_sec) {
        if (m_out)
          *m_out << "k-induction: timeout after " << elapsed << "s; result: unknown\n";
        return false;
      }
    }
    if (k_max > 0 && k > k_max) {
      if (m_out)
        *m_out << "k-induction: k_max=" << k_max << " reached; result: unknown\n";
      return false;
    }

    // One more peel, then re-run analyses and PathBMC.
    innerPM.run(M);

    solver::SolverResult res = PathBmcRunnerPass::getLastResult();

    if (res == solver::SolverResult::SAT) {
      if (m_out)
        *m_out << "sat\n";
      Stats::sset("Result", "FALSE");
      return false; // BUG found
    }
    if (res == solver::SolverResult::UNSAT) {
      if (m_out)
        *m_out << "unsat\n";
      Stats::sset("Result", "TRUE");
      return false; // SAFE
    }
    // UNKNOWN: continue with next k or give up
  }
}

KInductionResult KInductionEngine::run(llvm::Module &M) {
  // Engine run() is intended for programmatic use; the pass is the main entry.
  // We could run the same logic here with a minimal pass manager, but the
  // pass is the production entry point.
  (void)M;
  return KInductionResult::UNKNOWN;
}

} // namespace seahorn
#endif
