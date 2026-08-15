/// @file ICFGBuilder.h
/// @brief Builder class for constructing ICFG from LLVM modules.

#pragma once

#include "IR/ICFG/ICFG.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/InstrTypes.h>

/// @brief Possible callees supplied for an indirect call site.
struct ICFGCalleeTargets {
  llvm::SmallVector<const llvm::Function *, 8> targets;
  bool complete = false;
};

/// @brief Pluggable indirect-call target provider for ICFG construction.
class ICFGCalleeProvider {
public:
  virtual ~ICFGCalleeProvider() = default;

  /// Returns possible targets and whether the set is known to be complete.
  virtual ICFGCalleeTargets getTargets(const llvm::CallBase &call) const = 0;
};

/// @brief Constructs an ICFG from an LLVM module.
///
/// Processes all functions in a module to build intraprocedural and
/// interprocedural control flow edges.
class ICFGBuilder {
private:
  ICFG *icfg;
  const ICFGCalleeProvider *calleeProvider;

public:
  /// @brief Constructs an ICFG builder.
  /// @param i ICFG to populate.
  /// @param provider Optional indirect-call target provider. When omitted, a
  /// conservative signature-based resolver supplies defined candidates.
  explicit ICFGBuilder(ICFG *i, const ICFGCalleeProvider *provider = nullptr)
      : icfg(i), calleeProvider(provider) {}

  /// @brief Builds the ICFG for all functions in the module.
  /// @param module LLVM module to process.
  void build(llvm::Module *module);

  bool _removeCycleAfterBuild =
      false; ///< Flag to remove cycles after building.

public:
  /// @brief Sets whether to remove cycles after building the ICFG.
  /// @param b True to remove cycles.
  void setRemoveCycleAfterBuild(bool b);

private:
  /// @brief Processes a single function to create ICFG nodes and edges.
  /// @param func Function to process.
  void processFunction(const llvm::Function *func);

  /// @brief Gets or creates an ICFG node for a basic block.
  /// @param bb Basic block.
  /// @return Pointer to the ICFG node.
  IntraBlockNode *getOrAddIntraBlockICFGNode(const llvm::BasicBlock *bb) {
    return icfg->getIntraBlockNode(bb);
  }

  /// @brief Removes interprocedural back edges (recursive calls).
  void removeInterCallCycle();

  /// @brief Removes intraprocedural back edges (loops).
  void removeIntraBlockCycle();
};
