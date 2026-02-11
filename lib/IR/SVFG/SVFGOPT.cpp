#include "IR/SVFG/SVFGOPT.h"

#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGSerializer.h"

#include <vector>

using namespace lotus::analysis;

SVFG *SVFGOPT::buildAndOptimize(const ICFG *icfg,
                                const SVFGBuilderConfig &config) {
  SVFGBuilder builder(config);
  return builder.build(icfg);
}

void SVFGOPT::readAndOptimize(const std::string &filename) {
  SVFGSerializer::readText(*this, filename);
  optimize();
}

void SVFGOPT::buildAndWrite(const std::string &filename) {
  SVFGSerializer::writeText(*this, filename);
}

void SVFGOPT::optimize() {
  handleInterValueFlow();
  handleIntraValueFlow();
}

void SVFGOPT::connectAParamAndFParam(const llvm::CallBase *,
                                     const llvm::Argument *,
                                     const llvm::CallBase *, uint32_t) {}

void SVFGOPT::connectFRetAndARet(const llvm::Value *, const llvm::CallBase *,
                                 uint32_t) {}

void SVFGOPT::connectAInAndFIn(const ActualInSVFGNode *,
                               const FormalInSVFGNode *, uint32_t) {}

void SVFGOPT::connectFOutAndAOut(const FormalOutSVFGNode *,
                                 const ActualOutSVFGNode *, uint32_t) {}

void SVFGOPT::handleInterValueFlow() {
  // Lightweight interprocedural cleanup:
  // - Bypass trivial ActualParm nodes when they are pure forwarding nodes
  //   (single incoming edge, only call-related outgoing edges).
  // This is intentionally conservative and does not attempt to reproduce the
  // full upstream SVF SVFGOPT transformations.
  std::vector<ActualParmSVFGNode *> candidates;
  candidates.reserve(getNumNodes());
  for (auto &pair : *this) {
    if (auto *ap = llvm::dyn_cast<ActualParmSVFGNode>(pair.second)) {
      candidates.push_back(ap);
    }
  }

  for (ActualParmSVFGNode *ap : candidates) {
    if (!ap)
      continue;
    if (ap->getInEdges().size() != 1)
      continue;

    SVFGEdge *inE = ap->getInEdges().front();
    if (!inE || inE->getSrcNode() == nullptr)
      continue;

    // Only bypass when incoming is an intra edge (typical: IntraCopy).
    if (!inE->isIntraEdge())
      continue;

    SVFGNode *src = inE->getSrcNode();
    const auto outEdgesCopy = ap->getOutEdges();
    if (outEdgesCopy.empty())
      continue;

    bool onlyCallish = true;
    for (SVFGEdge *outE : outEdgesCopy) {
      if (!outE)
        continue;
      if (!(outE->isCallEdge() || outE->getEdgeKind() == SVFGEdgeK::ParamCall ||
            outE->getEdgeKind() == SVFGEdgeK::CallDir ||
            outE->getEdgeKind() == SVFGEdgeK::CallInd)) {
        onlyCallish = false;
        break;
      }
    }
    if (!onlyCallish)
      continue;

    // Retarget src -> dst for each outgoing edge, then delete the forwarding
    // edges and the single incoming edge.
    for (SVFGEdge *outE : outEdgesCopy) {
      if (!outE)
        continue;
      SVFGNode *dst = outE->getDstNode();
      if (!dst || dst == src)
        continue;
      addEdge(src, dst, outE->getEdgeKind(), outE->getCallSite(),
              outE->getPointsTo());
      removeEdge(outE);
    }
    removeEdge(inE);
  }
}

void SVFGOPT::replaceFParamWithPHI(PhiSVFGNode *, SVFGNode *) {}

void SVFGOPT::replaceARetWithPHI(PhiSVFGNode *, SVFGNode *) {}

void SVFGOPT::retargetEdgesOfAInFOut(SVFGNode *) {}

void SVFGOPT::retargetEdgesOfAOutFIn(SVFGNode *) {}

void SVFGOPT::handleIntraValueFlow() {
  // Bypass trivial memory PHI nodes to reduce graph size.
  // A PHI is considered trivial if it has exactly one incoming and one outgoing
  // edge and neither edge is interprocedural.
  std::vector<MSSAPhiSVFGNode *> phis;
  phis.reserve(getNumNodes());
  for (auto &pair : *this) {
    if (auto *phi = llvm::dyn_cast<MSSAPhiSVFGNode>(pair.second)) {
      phis.push_back(phi);
    }
  }
  for (const MSSAPhiSVFGNode *phi : phis) {
    bypassMSSAPHINode(phi);
  }
}

void SVFGOPT::bypassMSSAPHINode(const MSSAPhiSVFGNode *node) {
  if (!node)
    return;
  auto *phi = const_cast<MSSAPhiSVFGNode *>(node);
  if (phi->getInEdges().size() != 1 || phi->getOutEdges().size() != 1)
    return;

  SVFGEdge *inE = phi->getInEdges().front();
  SVFGEdge *outE = phi->getOutEdges().front();
  if (!inE || !outE)
    return;
  if (!inE->isIntraEdge() || !outE->isIntraEdge())
    return;

  SVFGNode *src = inE->getSrcNode();
  SVFGNode *dst = outE->getDstNode();
  if (!src || !dst || src == phi || dst == phi || src == dst)
    return;

  // Preserve points-to label information if present.
  SVFGNodeBS pts = inE->getPointsTo();
  pts.insert(outE->getPointsTo().begin(), outE->getPointsTo().end());

  addEdge(src, dst, outE->getEdgeKind(), outE->getCallSite(), pts);
  removeEdge(inE);
  removeEdge(outE);
}

bool SVFGOPT::handleSelfCycleEdges(const MSSAPhiSVFGNode *) { return false; }

void SVFGOPT::initialWorkList() {}

bool SVFGOPT::addToWorkList(const SVFGNode *) { return false; }

bool SVFGOPT::canRemoveNode(const SVFGNode *) { return false; }

void SVFGOPT::removeAllEdges(const SVFGNode *node) {
  removeIncomingEdges(node);
  removeOutgoingEdges(node);
}

void SVFGOPT::removeIncomingEdges(const SVFGNode *node) {
  if (!node)
    return;
  auto *n = const_cast<SVFGNode *>(node);
  const auto inEdgesCopy = n->getInEdges();
  for (SVFGEdge *edge : inEdgesCopy) {
    removeEdge(edge);
  }
}

void SVFGOPT::removeOutgoingEdges(const SVFGNode *node) {
  if (!node)
    return;
  auto *n = const_cast<SVFGNode *>(node);
  const auto outEdgesCopy = n->getOutEdges();
  for (SVFGEdge *edge : outEdgesCopy) {
    removeEdge(edge);
  }
}

bool SVFGOPT::addNewEdge(uint32_t, uint32_t, const SVFGEdge *,
                         const SVFGEdge *) {
  return false;
}

bool SVFGOPT::bothInterEdges(const SVFGEdge *, const SVFGEdge *) const {
  return false;
}

void SVFGOPT::addPHIOperand(PhiSVFGNode *, uint32_t, const llvm::Value *) {}

InterPhiSVFGNode *
SVFGOPT::addInterPHIForFormalParm(const FormalParmSVFGNode *) {
  return nullptr;
}

InterPhiSVFGNode *SVFGOPT::addInterPHIForActualRet(const ActualRetSVFGNode *) {
  return nullptr;
}

void SVFGOPT::resetDef(const llvm::Value *, SVFGNode *) {}

void SVFGOPT::setActualInDef(uint32_t, uint32_t) {}

void SVFGOPT::setFormalOutDef(uint32_t, uint32_t) {}

bool SVFGOPT::isDefOfAInFOut(const SVFGNode *) const { return false; }

bool SVFGOPT::actualInOfIndCS(const ActualInSVFGNode *) const { return false; }

bool SVFGOPT::actualOutOfIndCS(const ActualOutSVFGNode *) const {
  return false;
}

bool SVFGOPT::formalInOfAddressTakenFunc(const FormalInSVFGNode *) const {
  return false;
}

bool SVFGOPT::formalOutOfAddressTakenFunc(const FormalOutSVFGNode *) const {
  return false;
}

bool SVFGOPT::isConnectingTwoCallSites(const SVFGNode *) const { return false; }
