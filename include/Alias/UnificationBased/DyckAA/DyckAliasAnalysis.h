/**
 * @file DyckAliasAnalysis.h
 * @brief Dyck-CFL alias analysis using unification-based approach
 *
 * Canary features a fast unification-based alias analysis for C programs.
 * This analysis uses Dyck-CFL (Dyck context-free language) reachability
 * to compute alias sets and provide precise alias information.
 *
 * @author Qingkai Shi <qingkaishi@gmail.com>
 * @author Lotus Analysis Framework
 */

/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef ALIAS_DYCKAA_DYCKALIASANALYSIS_H
#define ALIAS_DYCKAA_DYCKALIASANALYSIS_H

#include "Alias/UnificationBased/DyckAA/DyckCallGraph.h"
#include "Alias/UnificationBased/DyckAA/DyckGraph.h"
#include "Alias/UnificationBased/DyckAA/DyckGraphEdgeLabel.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

/// @brief Dyck-CFL based alias analysis
///
/// Performs fast unification-based alias analysis using Dyck-CFL
/// reachability. Provides alias set queries and may-alias checks
/// for pointer analysis.
class DyckAliasAnalysis : public ModulePass {
private:
  DyckGraph *DyckPTG;
  DyckCallGraph *DyckCG;

public:
  static char ID;

  DyckAliasAnalysis();

  ~DyckAliasAnalysis() override;

  /// @brief Run the analysis on a module
  /// @param M The module to analyze
  /// @return true if analysis completed
  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// @brief Get alias set of a pointer
  /// @param Ptr The pointer to query
  /// @return Set of aliased values, or nullptr if no alias information
  const std::set<Value *> *getAliasSet(Value *Ptr) const;

  /// @brief Check if two values may alias
  /// @param V1 First value
  /// @param V2 Second value
  /// @return true if V1 may alias V2
  bool mayAlias(Value *V1, Value *V2) const;

  /// @brief Check if a value may be null
  /// @param V The value to check
  /// @return true if V may alias nullptr
  bool mayNull(Value *V) const;

  /// @brief Get the Dyck call graph
  /// @return Pointer to the call graph
  DyckCallGraph *getDyckCallGraph() const;

  /// @brief Get the Dyck-CFL graph
  /// @return Pointer to the points-to graph
  DyckGraph *getDyckGraph() const;

private:
  /// @brief Print alias set information for debugging
  ///
  /// Three kinds of information will be printed:
  /// 1. Alias Sets will be printed to the console
  /// 2. The relation of Alias Sets will be output into "alias_rel.dot"
  /// 3. The evaluation results will be output into "distribution.log"
  ///    The summary of the evaluation will be printed to the console
  void printAliasSetInformation();
};

#endif // ALIAS_DYCKAA_DYCKALIASANALYSIS_H
