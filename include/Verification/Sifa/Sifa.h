//===-- Verification/Sifa/Sifa.h ------------------------------------------===//
//
// Intraprocedural Sifa (Symbolic Interpretation with Fluid Abstractions)
// skeleton for lotus.
//
// v1 milestone: build CFG -> path expressions -> interpret regex using a domain.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SIFA_H
#define LOTUS_VERIFICATION_SIFA_SIFA_H

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Domain/ReachabilityDomain.h"
#include "Verification/Sifa/Interpreter/RegexInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureGraph.h"

#include "Utils/General/PathExpressions/PathExpressionComputer.h"

#include <cstddef>

// Needed for getEntryBlock() in the header-only analyzeTo().
#include "llvm/IR/Function.h"

namespace llvm {
class BasicBlock;
class Function;
class Module;
} // namespace llvm

namespace lotus {
namespace sifa {

struct SifaOptions {
  InterpreterOptions interpreter;
};

/// Analyze all paths from entry to \p target and return the resulting abstract state.
template <typename StateT>
StateT analyzeTo(const llvm::Function &F, const llvm::BasicBlock &target, const StateT &initial,
                 const AbstractDomain<Transition, StateT> &domain,
                 SifaOptions options = {}) {
  const ProcedureGraph pg(F);
  auto *entry = const_cast<llvm::BasicBlock *>(&F.getEntryBlock());
  auto *tgt = const_cast<llvm::BasicBlock *>(&target);

  lotus::pathexpressions::PathExpressionComputer<ProcedureGraph::Node, Transition> comp(
      pg.graph());
  auto expr = comp.exprBetween(entry, tgt);

  RegexInterpreter<Transition, StateT> interp(domain, options.interpreter);
  return interp.eval(expr, initial);
}

/// Convenience API: procedure-level reachability via Sifa's path-expression interpreter.
bool isReachable(const llvm::Function &F, const llvm::BasicBlock &target,
                 SifaOptions options = {});

/// Interprocedural reachability: \p targetBlock in \p targetFunc reachable from \p entry.
bool isReachableInterprocedural(const llvm::Module &M, const llvm::Function *entry,
                               const llvm::Function &targetFunc,
                               const llvm::BasicBlock &targetBlock,
                               SifaOptions options = {});

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SIFA_H
