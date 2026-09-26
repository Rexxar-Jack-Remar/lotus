/// cclyzer++ (Datalog-based) alias analysis backend for Lotus.
/// Backed by the in-tree third-party/cclyzerpp (GaloisInc/cclyzerpp).
/// When built with LOTUS_ENABLE_CCLYZER=ON and Soufflé available, provides
/// scalable, field-sensitive and context-sensitive Datalog pointer analysis.
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace cclyzer {

/// Supported pointer-analysis variants in cclyzer++
enum class AnalysisKind {
  Subset,      ///< Andersen-style inclusion-based analysis
  Unification, ///< Steensgaard-style unification-based analysis
  Debug        ///< Runs both subset and unification for cross-verification
};

/// Context sensitivity modes
enum class ContextKind {
  Insensitive,
  CallSite1,
  CallSite2,
  CallSite3,
  CallSite4,
  CallSite5,
  CallSite6,
  CallSite7,
  CallSite8,
  CallSite9,
  Caller1,
  Caller2,
  Caller3,
  Caller4,
  Caller5,
  Caller6,
  Caller7,
  Caller8,
  Caller9
};

/// Configuration options for CclyzerAA execution
struct CclyzerOptions {
  AnalysisKind analysis = AnalysisKind::Subset;
  ContextKind context = ContextKind::Insensitive;
  std::string signaturesPath;
  std::string debugDir;
  bool checkAssertions = false;
};

/// Standalone wrapper around cclyzer++ pointer analysis.
/// Run analysis on a module, then query alias, points-to, call graph, etc.
class CclyzerAA {
public:
  CclyzerAA();
  explicit CclyzerAA(const CclyzerOptions &options);
  ~CclyzerAA();

  /// Set analysis configuration options.
  void setOptions(const CclyzerOptions &options);

  /// Get current analysis configuration options.
  [[nodiscard]] const CclyzerOptions &getOptions() const;

  /// Check whether the cclyzer++ backend is compiled and available in this build.
  static bool isAvailable();

  /// Run Datalog-based pointer analysis on \p M using current options.
  /// Returns true if analysis succeeded, false otherwise.
  bool run(llvm::Module &M);

  /// Run Datalog-based pointer analysis on \p M using \p options.
  bool run(llvm::Module &M, const CclyzerOptions &options);

  /// Query alias between two pointers. Call after run().
  llvm::AliasResult alias(const llvm::Value *v1, const llvm::Value *v2);

  /// Query alias between two memory locations.
  llvm::AliasResult alias(const llvm::MemoryLocation &loc1,
                          const llvm::MemoryLocation &loc2);

  /// Fast boolean queries
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2);
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2);

  /// Get points-to set for pointer \p ptr. Call after run().
  /// Returns true if the backend supports points-to and \p ptsSet was filled.
  bool getPointsToSet(const llvm::Value *ptr,
                      std::vector<const llvm::Value *> &ptsSet);

  /// Check whether \p ptr is determined to point to null.
  bool isNullPointer(const llvm::Value *ptr) const;

  /// Get all identified null pointers.
  bool getNullPtrSet(std::set<const llvm::Value *> &nulls) const;

  /// Resolve indirect call targets for \p callSite.
  bool getIndirectCallTargets(const llvm::Instruction *callSite,
                              std::vector<const llvm::Value *> &targets) const;

  /// Whether run() completed successfully and queries are valid.
  [[nodiscard]] bool isInitialized() const { return _initialized; }

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  CclyzerOptions _options;
  bool _initialized = false;
};

} // namespace cclyzer
} // namespace lotus
