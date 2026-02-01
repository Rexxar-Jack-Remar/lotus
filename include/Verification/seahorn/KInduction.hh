/*

k-induction verification that reuses Seahorn PathBMC.

For each k = KMin, KMin+1, ... (until KMax or timeout):
1. Peel all loops (k+1) times (incrementally: peel once per iteration).
2. Run PathBMC on the peeled program.
3. If SAT → return BUG (counterexample in ≤ k+1 steps).
4. If UNSAT → return SAFE (no error in ≤ k+1 steps).

@author: rainoftime

*/
#pragma once

#include "seahorn/Expr/Expr.hh"
#include "seahorn/Expr/Smt/Solver.hh"

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {
class Module;
class Function;
class raw_ostream;
} // namespace llvm

namespace seahorn {

/// Result of k-induction verification.
enum class KInductionResult {
  /// No error reachable within the explored bound (sound proof).
  SAFE,
  /// Counterexample found (bug).
  BUG,
  /// Timeout, solver error, or inconclusive.
  UNKNOWN
};

/// Options for the k-induction engine.
struct KInductionOptions {
  /// Minimum k to try (default 1).
  unsigned KMin = 1;
  /// Maximum k (0 = no limit; stop on timeout or SAFE).
  unsigned KMax = 0;
  /// Total CPU timeout in seconds (0 = no timeout).
  unsigned TimeoutSec = 0;
  /// Entry function name (default "main").
  std::string EntryName = "main";
};

/// Production-ready k-induction engine that reuses Seahorn PathBMC.
///
/// For each k = KMin, KMin+1, ... (until KMax or timeout):
/// 1. Peel all loops (k+1) times (incrementally: peel once per iteration).
/// 2. Run PathBMC on the peeled program.
/// 3. If SAT → return BUG (counterexample in ≤ k+1 steps).
/// 4. If UNSAT → return SAFE (no error in ≤ k+1 steps).
///
/// Requires the module to be preprocessed (ShadowMem, etc.) and CLAM
/// for full PathBMC; otherwise falls back to UNKNOWN.
class KInductionEngine {
public:
  KInductionEngine() = default;
  explicit KInductionEngine(KInductionOptions opts) : m_opts(std::move(opts)) {}

  void setOptions(KInductionOptions opts) { m_opts = std::move(opts); }
  const KInductionOptions &getOptions() const { return m_opts; }

  /// Run k-induction on the module. Entry function must exist and be
  /// preprocessed (e.g. after seahorn pipeline up to BMC).
  /// Returns SAFE, BUG, or UNKNOWN.
  KInductionResult run(llvm::Module &M);

  /// Last k for which step case was checked (0 if none).
  unsigned getLastK() const { return m_lastK; }

private:
  KInductionOptions m_opts;
  unsigned m_lastK = 0;
};

/// LLVM legacy module pass: runs k-induction and prints result to \p out.
/// Use in the same pipeline as PathBmcPass (after preprocessing).
class KInductionPass : public llvm::ModulePass {
public:
  static char ID;

  KInductionPass(llvm::raw_ostream *out = nullptr);
  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  llvm::StringRef getPassName() const override {
    return "K-Induction (PathBMC-based)";
  }

private:
  llvm::raw_ostream *m_out;
};

/// Create the k-induction pass (legacy PM).
llvm::Pass *createKInductionPass(llvm::raw_ostream *out = nullptr);

} // namespace seahorn
