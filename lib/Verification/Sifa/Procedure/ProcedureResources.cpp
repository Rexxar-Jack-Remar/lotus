#include "Verification/Sifa/Procedure/ProcedureResources.h"

#include "Utils/Algorithms/PathExpressions/Regex.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/RegexDag/RegexDagUtils.h"
#include "Verification/Sifa/Statistics/RegexStatUtils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <unordered_set>

using namespace lotus::sifa;

ProcedureResources::ProcedureResources(SifaStats &stats, const llvm::Function &F,
                                       const std::vector<llvm::BasicBlock *> &lois)
    : ProcedureResources(stats, F, lois, {}) {}

ProcedureResources::ProcedureResources(SifaStats &stats, const llvm::Function &F,
                                       const std::vector<llvm::BasicBlock *> &lois,
                                       const std::vector<const llvm::Function *> &enterCallsOfInterest) {
  const ProcedureGraph pg(F);
  auto *entry = const_cast<llvm::BasicBlock *>(&F.getEntryBlock());

  auto pe = createPEComputer(stats, pg.graph());
  auto regexToDag = createRegexToDag<Transition>(stats);

  std::vector<RegexDagNode<Transition> *> loiMarkers;
  loiMarkers.reserve(lois.size());

  // Marker ids must be stable and unique within the DAG's transition alphabet.
  // We start at 1 to avoid the common "0 means uninitialized" convention.
  std::uint32_t nextMarkerId = 1;
  for (llvm::BasicBlock *loi : lois) {
    auto expr = exprBetween(stats, pe, entry, loi);
    auto marked = markRegex(expr, loi, nextMarkerId++);
    loiMarkers.push_back(addToDag(stats, regexToDag, marked));
  }

  // Also add one marked regex to the explicit EXIT node (nullptr).
  // ProcedureGraph ensures EXIT is always reachable from every return block via
  // an outgoing edge to nullptr, so the path-expression computer can compute an
  // (entry -> EXIT) expression uniformly.
  llvm::BasicBlock *const exitNode = nullptr;
  auto exprToExit = exprBetween(stats, pe, entry, exitNode);
  auto markedExit = markRegex(exprToExit, /*finalLocationAsMark=*/nullptr, nextMarkerId++);
  auto *exitMarker = addToDag(stats, regexToDag, markedExit);

  std::vector<RegexDagNode<Transition> *> enterCallMarkers;
  if (!enterCallsOfInterest.empty()) {
    std::unordered_set<const llvm::Function *> enterCallsSet(enterCallsOfInterest.begin(),
                                                            enterCallsOfInterest.end());
    for (const auto &edgePtr : pg.graph().getEdges()) {
      const Transition &t = edgePtr->getLabel();
      if (t.kind != TransitionKind::ReturnSummary || !t.callee ||
          enterCallsSet.find(t.callee) == enterCallsSet.end()) {
        continue;
      }
      auto pathExpr = exprBetween(stats, pe, entry, edgePtr->getSource());
      auto lit = lotus::pathexpressions::Regex<Transition>::literal(t);
      auto concat = lotus::pathexpressions::Regex<Transition>::concat(pathExpr, lit);
      enterCallMarkers.push_back(addToDag(stats, regexToDag, concat));
    }
  }

  regexDag_ = getDagAndReset(stats, regexToDag);
  compress(stats, regexDag_);

  for (auto *m : loiMarkers) {
    overlayToLois_.addInclusive(m);
  }

  overlayToReturn_.addInclusive(exitMarker);

  for (auto *m : loiMarkers) {
    overlayToLoisAndReturn_.addInclusive(m);
  }
  overlayToLoisAndReturn_.addInclusive(exitMarker);
  for (auto *m : enterCallMarkers) {
    overlayToLoisAndReturn_.addExclusive(m);
  }

  // overlayToLoisAndEnterCalls_: LOI markers (inclusive) + enter-call markers
  // (exclusive). Does NOT include the EXIT marker — the interpreter stops at
  // call sites rather than propagating through to return.
  for (auto *m : loiMarkers) {
    overlayToLoisAndEnterCalls_.addInclusive(m);
  }
  for (auto *m : enterCallMarkers) {
    overlayToLoisAndEnterCalls_.addExclusive(m);
  }
}

const RegexDag<Transition> &ProcedureResources::getRegexDag() const { return regexDag_; }
const BackwardClosedOverlay<Transition> &ProcedureResources::getDagOverlayPathToLois() const {
  return overlayToLois_;
}
const BackwardClosedOverlay<Transition> &ProcedureResources::getDagOverlayPathToReturn() const {
  return overlayToReturn_;
}
const BackwardClosedOverlay<Transition> &ProcedureResources::getDagOverlayPathToLoisAndReturn() const {
  return overlayToLoisAndReturn_;
}
const BackwardClosedOverlay<Transition> &ProcedureResources::getDagOverlayPathToLoisAndEnterCalls() const {
  return overlayToLoisAndEnterCalls_;
}
