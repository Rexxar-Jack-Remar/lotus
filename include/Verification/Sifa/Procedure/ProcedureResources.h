//===-- Verification/Sifa/Procedure/ProcedureResources.h ------------------===//
//
// Procedure-level resources: RegexDag and overlays (ported from Ultimate Sifa).
//
// This is the LLVM intraprocedural analogue of Java's ProcedureResources.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDURERESOURCES_H
#define LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDURERESOURCES_H

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Procedure/ProcedureGraph.h"
#include "Verification/Sifa/RegexDag/BackwardClosedOverlay.h"
#include "Verification/Sifa/RegexDag/RegexDag.h"
#include "Verification/Sifa/Statistics/SifaStats.h"

#include <vector>

namespace llvm {
class BasicBlock;
class Function;
} // namespace llvm

namespace lotus {
namespace sifa {

class ProcedureResources {
public:
  /// Build resources with LOIs only (no enter-call markers in overlay).
  ProcedureResources(SifaStats &stats, const llvm::Function &F,
                     const std::vector<llvm::BasicBlock *> &locationsOfInterest);

  /// Ultimate-aligned: LOIs + \p enterCallsOfInterest (callees). Overlay includes
  /// paths to LOIs (inclusive), to return (inclusive), and to enter-calls (exclusive).
  ProcedureResources(SifaStats &stats, const llvm::Function &F,
                     const std::vector<llvm::BasicBlock *> &locationsOfInterest,
                     const std::vector<const llvm::Function *> &enterCallsOfInterest);

  const RegexDag<Transition> &getRegexDag() const;
  const BackwardClosedOverlay<Transition> &getDagOverlayPathToLois() const;
  const BackwardClosedOverlay<Transition> &getDagOverlayPathToReturn() const;
  /// Overlay including both LOI markers and return (for interprocedural interpretWithCalls).
  const BackwardClosedOverlay<Transition> &getDagOverlayPathToLoisAndReturn() const;
  /// Ultimate-aligned name: overlay for LOIs and enter-calls (same as PathToLoisAndReturn).
  const BackwardClosedOverlay<Transition> &getDagOverlayPathToLoisAndEnterCalls() const;

private:
  RegexDag<Transition> regexDag_;
  BackwardClosedOverlay<Transition> overlayToLois_;
  BackwardClosedOverlay<Transition> overlayToReturn_;
  BackwardClosedOverlay<Transition> overlayToLoisAndReturn_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDURERESOURCES_H
