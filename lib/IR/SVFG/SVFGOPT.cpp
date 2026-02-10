#include "IR/SVFG/SVFGOPT.h"

#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGSerializer.h"

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

void SVFGOPT::handleInterValueFlow() {}

void SVFGOPT::replaceFParamWithPHI(PhiSVFGNode *, SVFGNode *) {}

void SVFGOPT::replaceARetWithPHI(PhiSVFGNode *, SVFGNode *) {}

void SVFGOPT::retargetEdgesOfAInFOut(SVFGNode *) {}

void SVFGOPT::retargetEdgesOfAOutFIn(SVFGNode *) {}

void SVFGOPT::handleIntraValueFlow() {}

void SVFGOPT::bypassMSSAPHINode(const MSSAPhiSVFGNode *) {}

bool SVFGOPT::handleSelfCycleEdges(const MSSAPhiSVFGNode *) { return false; }

void SVFGOPT::initialWorkList() {}

bool SVFGOPT::addToWorkList(const SVFGNode *) { return false; }

bool SVFGOPT::canRemoveNode(const SVFGNode *) { return false; }

void SVFGOPT::removeAllEdges(const SVFGNode *) {}

void SVFGOPT::removeIncomingEdges(const SVFGNode *) {}

void SVFGOPT::removeOutgoingEdges(const SVFGNode *) {}

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
