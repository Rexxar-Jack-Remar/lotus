//===-- Verification/Sifa/Procedure/ProcedureGraphBuilder.h ----------------===//
//
// Builds procedure graphs from LLVM functions (ported from Ultimate Sifa).
//
// Optionally restricts the graph to nodes backward-reachable from exit and
// locations of interest. For future ICFG integration, enter-calls-of-interest
// can be supplied to add dead-end edges for "enter call without return".
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPHBUILDER_H
#define LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPHBUILDER_H

#include "Verification/Sifa/Procedure/ProcedureGraph.h"
#include "Verification/Sifa/Statistics/SifaStats.h"

#include <vector>

namespace llvm {
class BasicBlock;
class Function;
} // namespace llvm

namespace lotus {
namespace sifa {

/// Builds ProcedureGraph from an LLVM function, optionally restricted to
/// backward-reachable nodes from exit and locations of interest.
class ProcedureGraphBuilder {
public:
  using Node = ProcedureGraph::Node;

  explicit ProcedureGraphBuilder(SifaStats &stats, const llvm::Function &F);

  /// Build a procedure graph containing entry, exit, LOIs, and all nodes/edges
  /// on backward paths from exit and LOIs. If \p locationsOfInterest is empty
  /// and \p restrictToReachable is false, the graph is the full CFG.
  ProcedureGraph graphOfProcedure(
      const std::vector<llvm::BasicBlock *> &locationsOfInterest,
      bool restrictToReachable = true);

private:
  SifaStats &stats_;
  const llvm::Function &F_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPHBUILDER_H
