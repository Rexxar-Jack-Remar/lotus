#include "IR/SVFG/SVFGOPT.h"

#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGSerializer.h"

#include "llvm/Support/Casting.h"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

using namespace lotus::analysis;
using namespace llvm;

namespace {

static bool isInterIndirectKind(SVFGEdgeK kind) {
  return kind == SVFGEdgeK::CallInd || kind == SVFGEdgeK::RetInd;
}

static bool isCallLikeKind(SVFGEdgeK kind) {
  return kind == SVFGEdgeK::CallDir || kind == SVFGEdgeK::CallInd ||
         kind == SVFGEdgeK::ParamCall;
}

static bool isRetLikeKind(SVFGEdgeK kind) {
  return kind == SVFGEdgeK::RetDir || kind == SVFGEdgeK::RetInd ||
         kind == SVFGEdgeK::ParamRet;
}

static SVFGNodeBS intersectPts(const SVFGEdge *lhs, const SVFGEdge *rhs) {
  if (!lhs || !rhs)
    return {};
  SVFGNodeBS result;
  const SVFGNodeBS &a = lhs->getPointsTo();
  const SVFGNodeBS &b = rhs->getPointsTo();
  if (a.empty() || b.empty())
    return result;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::inserter(result, result.begin()));
  return result;
}

} // namespace

SVFG *SVFGOPT::buildAndOptimize(const ICFG *icfg,
                                const SVFGBuilderConfig &config) {
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> built(builder.build(icfg));
  if (!built) {
    return nullptr;
  }
  swapWith(*built);
  optimize();
  return this;
}

void SVFGOPT::readAndOptimize(const std::string &filename) {
  SVFGSerializer::readText(*this, filename);
  optimize();
}

void SVFGOPT::buildAndWrite(const std::string &filename) {
  optimize();
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
  SVFGNodeSet candidates;
  for (auto &pair : *this) {
    SVFGNode *node = pair.second;
    if (!node)
      continue;
    if (isa<ActualParmSVFGNode>(node) || isa<ActualRetSVFGNode>(node) ||
        isa<FormalParmSVFGNode>(node) || isa<FormalRetSVFGNode>(node) ||
        isa<ActualInSVFGNode>(node) || isa<ActualOutSVFGNode>(node) ||
        isa<FormalInSVFGNode>(node) || isa<FormalOutSVFGNode>(node)) {
      candidates.insert(node);
    }
  }

  SVFGNodeSet nodesToDelete;
  for (SVFGNode *node : candidates) {
    if (auto *fp = dyn_cast<FormalParmSVFGNode>(node)) {
      replaceFParamWithPHI(addInterPHIForFormalParm(fp), fp);
      nodesToDelete.insert(fp);
    } else if (auto *ar = dyn_cast<ActualRetSVFGNode>(node)) {
      replaceARetWithPHI(addInterPHIForActualRet(ar), ar);
      nodesToDelete.insert(ar);
    } else if (isa<ActualParmSVFGNode>(node) || isa<FormalRetSVFGNode>(node)) {
      nodesToDelete.insert(node);
    } else if (isa<ActualInSVFGNode>(node) || isa<FormalOutSVFGNode>(node)) {
      retargetEdgesOfAInFOut(node);
      nodesToDelete.insert(node);
    } else if ((isa<ActualOutSVFGNode>(node) || isa<FormalInSVFGNode>(node)) &&
               !keepActualOutFormalIn) {
      nodesToDelete.insert(node);
    }
  }

  for (SVFGNode *node : nodesToDelete) {
    if (!node || !hasNode(node->getId()))
      continue;
    if (!canRemoveNode(node))
      continue;
    if (isa<ActualOutSVFGNode>(node) || isa<FormalInSVFGNode>(node)) {
      retargetEdgesOfAOutFIn(node);
    }
    removeAllEdges(node);
    removeNode(node);
  }
}

void SVFGOPT::replaceFParamWithPHI(PhiSVFGNode *phi, SVFGNode *svfgNode) {
  if (!phi || !svfgNode)
    return;
  auto *formalParm = dyn_cast<FormalParmSVFGNode>(svfgNode);
  if (!formalParm)
    return;

  for (SVFGEdge *outEdge : svfgNode->getOutEdges()) {
    if (!outEdge || !outEdge->getDstNode())
      continue;
    addEdge(phi, outEdge->getDstNode(), outEdge->getEdgeKind(),
            outEdge->getCallSite(), outEdge->getPointsTo());
  }

  for (SVFGEdge *inEdge : svfgNode->getInEdges()) {
    auto *ap = dyn_cast<ActualParmSVFGNode>(inEdge ? inEdge->getSrcNode() : nullptr);
    if (!ap)
      continue;

    if (ap->getCallSite() && ap->getParamIndex() < ap->getCallSite()->arg_size()) {
      addPHIOperand(phi, ap->getParamIndex(),
                    ap->getCallSite()->getArgOperand(ap->getParamIndex()));
    }

    for (SVFGEdge *apIn : ap->getInEdges()) {
      if (!apIn || !apIn->getSrcNode())
        continue;
      SVFGEdgeK kind = inEdge->getEdgeKind();
      if (!isCallLikeKind(kind)) {
        kind = SVFGEdgeK::IntraCopy;
      }
      addEdge(apIn->getSrcNode(), phi, kind, inEdge->getCallSite(),
              apIn->getPointsTo());
    }
  }

  removeAllEdges(svfgNode);
}

void SVFGOPT::replaceARetWithPHI(PhiSVFGNode *phi, SVFGNode *svfgNode) {
  if (!phi || !svfgNode)
    return;
  auto *actualRet = dyn_cast<ActualRetSVFGNode>(svfgNode);
  if (!actualRet)
    return;

  for (SVFGEdge *outEdge : svfgNode->getOutEdges()) {
    if (!outEdge || !outEdge->getDstNode())
      continue;
    addEdge(phi, outEdge->getDstNode(), outEdge->getEdgeKind(),
            outEdge->getCallSite(), outEdge->getPointsTo());
  }

  for (SVFGEdge *inEdge : svfgNode->getInEdges()) {
    auto *fr = dyn_cast<FormalRetSVFGNode>(inEdge ? inEdge->getSrcNode() : nullptr);
    if (!fr)
      continue;

    addPHIOperand(phi, 0, fr->getValue());
    for (SVFGEdge *frIn : fr->getInEdges()) {
      if (!frIn || !frIn->getSrcNode())
        continue;
      SVFGEdgeK kind = inEdge->getEdgeKind();
      if (!isRetLikeKind(kind)) {
        kind = SVFGEdgeK::IntraCopy;
      }
      addEdge(frIn->getSrcNode(), phi, kind, inEdge->getCallSite(),
              frIn->getPointsTo());
    }
  }

  removeAllEdges(svfgNode);
}

void SVFGOPT::retargetEdgesOfAInFOut(SVFGNode *node) {
  if (!node || node->getInEdges().size() != 1)
    return;

  auto *inEdge = node->getInEdges().front();
  auto *def = inEdge ? inEdge->getSrcNode() : nullptr;
  if (!inEdge || !def)
    return;

  const SVFGNodeBS inPts = inEdge->getPointsTo();
  if (isa<ActualInSVFGNode>(node)) {
    setActualInDef(node->getId(), def->getId());
  } else if (isa<FormalOutSVFGNode>(node)) {
    setFormalOutDef(node->getId(), def->getId());
  }

  for (SVFGEdge *outEdge : node->getOutEdges()) {
    if (!outEdge || !outEdge->getDstNode())
      continue;
    SVFGNodeBS pts = inPts;
    if (!pts.empty() && !outEdge->getPointsTo().empty()) {
      SVFGNodeBS inter;
      std::set_intersection(pts.begin(), pts.end(), outEdge->getPointsTo().begin(),
                            outEdge->getPointsTo().end(),
                            std::inserter(inter, inter.begin()));
      pts = std::move(inter);
    } else {
      pts.clear();
    }
    if (inEdge->getPointsTo().empty() && outEdge->getPointsTo().empty()) {
      pts.clear();
    }
    if (!outEdge->getPointsTo().empty() && pts.empty())
      continue;
    addEdge(def, outEdge->getDstNode(), outEdge->getEdgeKind(),
            outEdge->getCallSite(), pts);
  }

  removeAllEdges(node);
}

void SVFGOPT::retargetEdgesOfAOutFIn(SVFGNode *node) {
  if (!node)
    return;

  const auto inEdges = node->getInEdges();
  const auto outEdges = node->getOutEdges();

  for (SVFGEdge *inEdge : inEdges) {
    if (!inEdge || !inEdge->getSrcNode())
      continue;
    for (SVFGEdge *outEdge : outEdges) {
      if (!outEdge || !outEdge->getDstNode())
        continue;
      SVFGNodeBS pts = intersectPts(inEdge, outEdge);
      if (!inEdge->getPointsTo().empty() && !outEdge->getPointsTo().empty() &&
          pts.empty()) {
        continue;
      }

      SVFGEdgeK kind = inEdge->getEdgeKind();
      if (!isInterIndirectKind(kind))
        kind = outEdge->getEdgeKind();
      if (!isIndirectVFGEdge(kind))
        kind = SVFGEdgeK::IntraIndirect;

      addEdge(inEdge->getSrcNode(), outEdge->getDstNode(), kind,
              inEdge->getCallSite() ? inEdge->getCallSite()
                                    : outEdge->getCallSite(),
              pts);
    }
  }

  removeAllEdges(node);
}

void SVFGOPT::handleIntraValueFlow() {
  initialWorkList();
  while (!workList.empty()) {
    const MSSAPhiSVFGNode *phi = workList.back();
    workList.pop_back();
    if (!phi || !hasNode(phi->getId()))
      continue;

    if (handleSelfCycleEdges(phi))
      continue;

    if (!phi->getInEdges().empty() && !phi->getOutEdges().empty()) {
      bypassMSSAPHINode(phi);
    }

    if (!phi->getInEdges().empty() && phi->getOutEdges().empty()) {
      for (SVFGEdge *inEdge : phi->getInEdges()) {
        if (inEdge && inEdge->getSrcNode()) {
          addToWorkList(inEdge->getSrcNode());
        }
      }
      removeIncomingEdges(phi);
    } else if (!phi->getOutEdges().empty() && phi->getInEdges().empty()) {
      for (SVFGEdge *outEdge : phi->getOutEdges()) {
        if (outEdge && outEdge->getDstNode()) {
          addToWorkList(outEdge->getDstNode());
        }
      }
      removeOutgoingEdges(phi);
    }

    if (phi->getInEdges().empty() && phi->getOutEdges().empty()) {
      removeNode(const_cast<MSSAPhiSVFGNode *>(phi));
    }
  }
}

void SVFGOPT::bypassMSSAPHINode(const MSSAPhiSVFGNode *node) {
  if (!node)
    return;

  const auto inEdges = node->getInEdges();
  const auto outEdges = node->getOutEdges();
  for (SVFGEdge *predEdge : inEdges) {
    if (!predEdge || !predEdge->getSrcNode())
      continue;
    bool added = false;
    for (SVFGEdge *succEdge : outEdges) {
      if (!succEdge || !succEdge->getDstNode())
        continue;
      if (predEdge->getSrcNode()->getId() == succEdge->getDstNode()->getId())
        continue;
      if (addNewEdge(predEdge->getSrcNode()->getId(),
                     succEdge->getDstNode()->getId(), predEdge, succEdge)) {
        added = true;
      } else {
        addToWorkList(succEdge->getDstNode());
      }
    }
    if (!added) {
      addToWorkList(predEdge->getSrcNode());
    }
  }
  removeAllEdges(node);
}

bool SVFGOPT::handleSelfCycleEdges(const MSSAPhiSVFGNode *node) {
  if (!node)
    return false;

  bool hasSelfCycle = false;
  const auto inEdges = node->getInEdges();
  for (SVFGEdge *edge : inEdges) {
    if (!edge || edge->getSrcNode() != edge->getDstNode())
      continue;

    if (keepAllSelfCycle) {
      hasSelfCycle = true;
      break;
    }

    if (keepContextSelfCycle &&
        (edge->getEdgeKind() == SVFGEdgeK::CallInd ||
         edge->getEdgeKind() == SVFGEdgeK::RetInd)) {
      hasSelfCycle = true;
      continue;
    }

    removeEdge(edge);
  }
  return hasSelfCycle;
}

void SVFGOPT::initialWorkList() {
  workList.clear();
  for (auto &pair : *this) {
    if (auto *phi = dyn_cast<MSSAPhiSVFGNode>(pair.second)) {
      workList.push_back(phi);
    }
  }
}

bool SVFGOPT::addToWorkList(const SVFGNode *node) {
  auto *phi = dyn_cast_or_null<MSSAPhiSVFGNode>(const_cast<SVFGNode *>(node));
  if (!phi)
    return false;
  if (!hasNode(phi->getId()))
    return false;
  for (const MSSAPhiSVFGNode *existing : workList) {
    if (existing == phi)
      return false;
  }
  workList.push_back(phi);
  return true;
}

bool SVFGOPT::canRemoveNode(const SVFGNode *node) {
  if (!node)
    return false;

  if (isa<ActualParmSVFGNode>(node) || isa<FormalParmSVFGNode>(node) ||
      isa<ActualRetSVFGNode>(node) || isa<FormalRetSVFGNode>(node)) {
    return true;
  }

  if (isa<ActualInSVFGNode>(node) || isa<ActualOutSVFGNode>(node) ||
      isa<FormalInSVFGNode>(node) || isa<FormalOutSVFGNode>(node) ||
      isa<MSSAPhiSVFGNode>(node)) {
    if (isConnectingTwoCallSites(node))
      return false;

    if (const auto *ai = dyn_cast<ActualInSVFGNode>(node)) {
      return !actualInOfIndCS(ai);
    }
    if (const auto *ao = dyn_cast<ActualOutSVFGNode>(node)) {
      return !actualOutOfIndCS(ao) && !isDefOfAInFOut(node);
    }
    if (const auto *fi = dyn_cast<FormalInSVFGNode>(node)) {
      return !formalInOfAddressTakenFunc(fi) && !isDefOfAInFOut(node);
    }
    if (const auto *fo = dyn_cast<FormalOutSVFGNode>(node)) {
      return !formalOutOfAddressTakenFunc(fo);
    }
  }

  return false;
}

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

bool SVFGOPT::addNewEdge(uint32_t srcId, uint32_t dstId, const SVFGEdge *predEdge,
                         const SVFGEdge *succEdge) {
  if (!predEdge || !succEdge)
    return false;
  SVFGNode *src = getNode(srcId);
  SVFGNode *dst = getNode(dstId);
  if (!src || !dst || src == dst)
    return false;

  SVFGNodeBS pts = intersectPts(predEdge, succEdge);
  if (!predEdge->getPointsTo().empty() && !succEdge->getPointsTo().empty() &&
      pts.empty()) {
    return false;
  }

  if (bothInterEdges(predEdge, succEdge))
    return false;

  SVFGEdgeK kind = SVFGEdgeK::IntraIndirect;
  const llvm::CallBase *cs = nullptr;
  if (predEdge->getEdgeKind() == SVFGEdgeK::CallInd ||
      succEdge->getEdgeKind() == SVFGEdgeK::CallInd) {
    kind = SVFGEdgeK::CallInd;
    cs = predEdge->getCallSite() ? predEdge->getCallSite()
                                 : succEdge->getCallSite();
  } else if (predEdge->getEdgeKind() == SVFGEdgeK::RetInd ||
             succEdge->getEdgeKind() == SVFGEdgeK::RetInd) {
    kind = SVFGEdgeK::RetInd;
    cs = predEdge->getCallSite() ? predEdge->getCallSite()
                                 : succEdge->getCallSite();
  }

  return addEdge(src, dst, kind, cs, pts) != nullptr;
}

bool SVFGOPT::bothInterEdges(const SVFGEdge *edge1,
                             const SVFGEdge *edge2) const {
  if (!edge1 || !edge2)
    return false;
  const bool e1Inter =
      edge1->getEdgeKind() == SVFGEdgeK::CallInd ||
      edge1->getEdgeKind() == SVFGEdgeK::RetInd;
  const bool e2Inter =
      edge2->getEdgeKind() == SVFGEdgeK::CallInd ||
      edge2->getEdgeKind() == SVFGEdgeK::RetInd;
  return e1Inter && e2Inter;
}

void SVFGOPT::addPHIOperand(PhiSVFGNode *, uint32_t, const llvm::Value *) {}

InterPhiSVFGNode *
SVFGOPT::addInterPHIForFormalParm(const FormalParmSVFGNode *formalParm) {
  uint32_t id = getNextNodeId();
  auto *phi = new InterPhiSVFGNode(id,
                                   formalParm ? formalParm->getICFGNode() : nullptr,
                                   formalParm ? formalParm->getFunction()
                                             : static_cast<const llvm::Function *>(nullptr));
  addNode(phi);
  return phi;
}

InterPhiSVFGNode *SVFGOPT::addInterPHIForActualRet(const ActualRetSVFGNode *actualRet) {
  uint32_t id = getNextNodeId();
  auto *phi = new InterPhiSVFGNode(id, actualRet ? actualRet->getICFGNode() : nullptr,
                                   actualRet ? actualRet->getCallSite()
                                             : static_cast<const llvm::CallBase *>(nullptr));
  addNode(phi);
  return phi;
}

void SVFGOPT::resetDef(const llvm::Value *value, SVFGNode *node) {
  if (!value || !node)
    return;
  setValueNode(value, node->getId());
}

void SVFGOPT::setActualInDef(uint32_t aiId, uint32_t defId) {
  actualInToDefMap[aiId] = defId;
  defNodes.insert(defId);
}

void SVFGOPT::setFormalOutDef(uint32_t foId, uint32_t defId) {
  formalOutToDefMap[foId] = defId;
  defNodes.insert(defId);
}

bool SVFGOPT::isDefOfAInFOut(const SVFGNode *node) const {
  if (!node)
    return false;
  return defNodes.count(node->getId()) != 0;
}

bool SVFGOPT::actualInOfIndCS(const ActualInSVFGNode *ai) const {
  if (!ai)
    return false;
  for (const SVFGEdge *edge : ai->getOutEdges()) {
    if (!edge)
      continue;
    if (edge->getEdgeKind() == SVFGEdgeK::CallInd)
      return true;
    if (!edge->getPointsTo().empty())
      return true;
  }
  return false;
}

bool SVFGOPT::actualOutOfIndCS(const ActualOutSVFGNode *ao) const {
  if (!ao)
    return false;
  for (const SVFGEdge *edge : ao->getInEdges()) {
    if (!edge)
      continue;
    if (edge->getEdgeKind() == SVFGEdgeK::RetInd)
      return true;
    if (!edge->getPointsTo().empty())
      return true;
  }
  return false;
}

bool SVFGOPT::formalInOfAddressTakenFunc(const FormalInSVFGNode *fi) const {
  if (!fi)
    return false;
  return !fi->getDefSVFVars().empty();
}

bool SVFGOPT::formalOutOfAddressTakenFunc(const FormalOutSVFGNode *fo) const {
  if (!fo)
    return false;
  return !fo->getDefSVFVars().empty();
}

bool SVFGOPT::isConnectingTwoCallSites(const SVFGNode *node) const {
  if (!node)
    return false;

  bool hasInCallRet = false;
  bool hasOutCallRet = false;
  for (const SVFGEdge *edge : node->getInEdges()) {
    if (!edge)
      continue;
    if (edge->getEdgeKind() == SVFGEdgeK::CallInd ||
        edge->getEdgeKind() == SVFGEdgeK::RetInd) {
      hasInCallRet = true;
      break;
    }
  }
  for (const SVFGEdge *edge : node->getOutEdges()) {
    if (!edge)
      continue;
    if (edge->getEdgeKind() == SVFGEdgeK::CallInd ||
        edge->getEdgeKind() == SVFGEdgeK::RetInd) {
      hasOutCallRet = true;
      break;
    }
  }
  return hasInCallRet && hasOutCallRet;
}
