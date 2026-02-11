//===- SVFG.cpp -- SVFG Implementation --------------------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

#include "IR/SVFG/SVFG.h"

#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGSerializer.h"

#include <queue>

using namespace lotus::analysis;
using namespace llvm;

const llvm::Function *SVFGNode::getFunction() const {
  if (icfgNode) {
    return icfgNode->getFunction();
  }
  return nullptr;
}

std::string SVFGNode::toString() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  os << "SVFGNode ID: " << getId()
     << " Kind: " << static_cast<uint32_t>(getNodeKind());
  if (const auto *val = getValue()) {
    os << " Value: " << val->getName().str();
  }
  return os.str();
}

std::string SVFGEdge::toString() const {
  std::string result;
  switch (kind) {
  case SVFGEdgeK::IntraCopy:
    result = "IntraCopy";
    break;
  case SVFGEdgeK::IntraDirect:
    result = "IntraDirect";
    break;
  case SVFGEdgeK::IntraLoad:
    result = "IntraLoad";
    break;
  case SVFGEdgeK::IntraStore:
    result = "IntraStore";
    break;
  case SVFGEdgeK::IntraGep:
    result = "IntraGep";
    break;
  case SVFGEdgeK::IntraPhi:
    result = "IntraPhi";
    break;
  case SVFGEdgeK::IntraCmp:
    result = "IntraCmp";
    break;
  case SVFGEdgeK::IntraBranch:
    result = "IntraBranch";
    break;
  case SVFGEdgeK::IntraMu:
    result = "IntraMu";
    break;
  case SVFGEdgeK::IntraChi:
    result = "IntraChi";
    break;
  case SVFGEdgeK::CallMu:
    result = "CallMu";
    break;
  case SVFGEdgeK::CallChi:
    result = "CallChi";
    break;
  case SVFGEdgeK::RetMu:
    result = "RetMu";
    break;
  case SVFGEdgeK::EntryChi:
    result = "EntryChi";
    break;
  case SVFGEdgeK::CallDir:
    result = "CallDir";
    break;
  case SVFGEdgeK::CallInd:
    result = "CallInd";
    break;
  case SVFGEdgeK::CallAIn:
    result = "CallAIn";
    break;
  case SVFGEdgeK::CallFIn:
    result = "CallFIn";
    break;
  case SVFGEdgeK::ParamCall:
    result = "ParamCall";
    break;
  case SVFGEdgeK::RetDir:
    result = "RetDir";
    break;
  case SVFGEdgeK::RetInd:
    result = "RetInd";
    break;
  case SVFGEdgeK::RetAOut:
    result = "RetAOut";
    break;
  case SVFGEdgeK::RetFOut:
    result = "RetFOut";
    break;
  case SVFGEdgeK::ParamRet:
    result = "ParamRet";
    break;
  case SVFGEdgeK::IntraIndirect:
    result = "IntraIndirect";
    break;
  case SVFGEdgeK::ThreadMHPIndirectVF:
    result = "ThreadMHPIndirectVF";
    break;
  case SVFGEdgeK::Variant:
    result = "Variant";
    break;
  default:
    result = "Unknown";
    break;
  }
  return result;
}

void SVFG::addNode(SVFGNode *node) {
  nodeMap[node->getId()] = node;
  if (node->getId() >= nextNodeId) {
    nextNodeId = node->getId() + 1;
  }
  if (node->isMemNode() && node->getMemReg() != 0) {
    setMSSADef(node->getMemReg(), node, node->getSSAVersion());
  }
  updateStat(node);
}

SVFGEdge *SVFG::addEdge(SVFGNode *src, SVFGNode *dst, SVFGEdgeK kind,
                        const llvm::CallBase *callSite,
                        const SVFGNodeBS &pointsTo,
                        std::string callSiteDebug) {
  if (!src || !dst)
    return nullptr;

  if (callSiteDebug.empty() && callSite) {
    llvm::raw_string_ostream os(callSiteDebug);
    os << callSite->getFunction()->getName();
    if (const Function *callee = callSite->getCalledFunction()) {
      os << "->" << callee->getName();
    } else {
      os << "->ind";
    }
    if (const DebugLoc &dl = callSite->getDebugLoc()) {
      os << "@" << dl.getLine() << ":" << dl.getCol();
    }
  }

  // Check for duplicate edge
  for (auto *existing : src->getOutEdges()) {
    if (existing->getDstNode() == dst && existing->getEdgeKind() == kind &&
        existing->getCallSite() == callSite &&
        existing->getCallSiteDebug() == callSiteDebug) {
      existing->addPointsTo(pointsTo);
      if (existing->getCallSiteDebug().empty() && !callSiteDebug.empty()) {
        existing->setCallSiteDebug(callSiteDebug);
      }
      return existing;
    }
  }

  SVFGEdge *edge = new SVFGEdge(src, dst, kind, SVFGEdge::EdgeWeight::One,
                                callSite, std::move(callSiteDebug), pointsTo);
  src->addOutEdge(edge);
  dst->addInEdge(edge);
  updateStat(edge);
  return edge;
}

void SVFG::removeEdge(SVFGEdge *edge) {
  if (!edge)
    return;

  SVFGNode *src = edge->getSrcNode();
  SVFGNode *dst = edge->getDstNode();

  if (src) {
    src->removeOutEdge(edge);
  }
  if (dst) {
    dst->removeInEdge(edge);
  }

  delete edge;
}

void SVFG::removeNode(SVFGNode *node) {
  if (!node)
    return;

  const uint32_t nodeId = node->getId();
  const llvm::Value *value = node->getValue();
  const llvm::Instruction *inst = node->getInstruction();
  const llvm::Function *fun = node->getFunction();

  for (auto it = mssaVerToNodeMap.begin(); it != mssaVerToNodeMap.end();) {
    if (it->second == node) {
      it = mssaVerToNodeMap.erase(it);
    } else {
      ++it;
    }
  }

  if (value) {
    auto it = valueToNodeMap.find(value);
    if (it != valueToNodeMap.end() && it->second == nodeId) {
      valueToNodeMap.erase(it);
    }
  }

  if (inst) {
    auto it = instToDefMap.find(inst);
    if (it != instToDefMap.end() && it->second == nodeId) {
      instToDefMap.erase(it);
    }
  }

  nodeFunctionDebug.erase(nodeId);
  nodeCallSiteDebug.erase(nodeId);

  if (auto *actualIn = dyn_cast<ActualInSVFGNode>(node)) {
    const llvm::CallBase *cs = actualIn->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualInMap.find(cs);
      if (mapIt != callSiteToActualInMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualInMap.erase(mapIt);
      }
    }
  } else if (auto *actualOut = dyn_cast<ActualOutSVFGNode>(node)) {
    const llvm::CallBase *cs = actualOut->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualOutMap.find(cs);
      if (mapIt != callSiteToActualOutMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualOutMap.erase(mapIt);
      }
    }
  } else if (auto *actualParm = dyn_cast<ActualParmSVFGNode>(node)) {
    const llvm::CallBase *cs = actualParm->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualParmMap.find(cs);
      if (mapIt != callSiteToActualParmMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualParmMap.erase(mapIt);
      }
    }
  } else if (auto *actualRet = dyn_cast<ActualRetSVFGNode>(node)) {
    const llvm::CallBase *cs = actualRet->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualRetMap.find(cs);
      if (mapIt != callSiteToActualRetMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualRetMap.erase(mapIt);
      }
    }
  } else if (dyn_cast<FormalInSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalInMap.find(fun);
      if (mapIt != funcToFormalInMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalInMap.erase(mapIt);
      }
    }
  } else if (dyn_cast<FormalOutSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalOutMap.find(fun);
      if (mapIt != funcToFormalOutMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalOutMap.erase(mapIt);
      }
    }
  } else if (dyn_cast<FormalParmSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalParmMap.find(fun);
      if (mapIt != funcToFormalParmMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalParmMap.erase(mapIt);
      }
    }
  } else if (dyn_cast<FormalRetSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalRetMap.find(fun);
      if (mapIt != funcToFormalRetMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalRetMap.erase(mapIt);
      }
    }
  }

  nodeMap.erase(nodeId);
  nodesForUpdate.erase(node);
  delete node;
}

void SVFG::updateStat(SVFGNode *node) {
  if (!node)
    return;

  stat.numNodes++;

  switch (node->getNodeKind()) {
  case SVFGK::Addr:
    stat.numAddrNodes++;
    break;
  case SVFGK::Copy:
    stat.numCopyNodes++;
    break;
  case SVFGK::Load:
    stat.numLoadNodes++;
    break;
  case SVFGK::Store:
    stat.numStoreNodes++;
    break;
  case SVFGK::Gep:
    stat.numGepNodes++;
    break;
  case SVFGK::Phi:
  case SVFGK::IntraPhi:
  case SVFGK::InterPhi:
    stat.numPhiNodes++;
    break;
  case SVFGK::FormalIn:
  case SVFGK::FormalOut:
  case SVFGK::ActualIn:
  case SVFGK::ActualOut:
  case SVFGK::MPhi:
  case SVFGK::MIntraPhi:
  case SVFGK::MInterPhi:
  case SVFGK::LoadMu:
  case SVFGK::StoreChi:
  case SVFGK::CallMu:
  case SVFGK::CallChi:
  case SVFGK::RetMu:
  case SVFGK::EntryChi:
    stat.numMemNodes++;
    break;
  case SVFGK::FormalParm:
  case SVFGK::ActualParm:
  case SVFGK::FormalRet:
  case SVFGK::ActualRet:
    stat.numParamNodes++;
    break;
  default:
    break;
  }
}

void SVFG::updateStat(SVFGEdge *edge) {
  if (!edge)
    return;

  stat.numEdges++;

  if (isCallVFGEdge(edge->getEdgeKind())) {
    stat.numCallEdges++;
  } else if (isRetVFGEdge(edge->getEdgeKind())) {
    stat.numRetEdges++;
  } else if (isIntraVFGEdge(edge->getEdgeKind())) {
    stat.numIntraEdges++;
  }
}

SVFGNodeSet SVFG::getPreds(SVFGNode *node) const {
  SVFGNodeSet result;
  if (!node)
    return result;

  std::queue<SVFGNode *> worklist;
  worklist.push(node);
  result.insert(node);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    for (auto *edge : current->getInEdges()) {
      SVFGNode *pred = edge->getSrcNode();
      if (result.insert(pred).second) {
        worklist.push(pred);
      }
    }
  }

  result.erase(node);
  return result;
}

SVFGNodeSet SVFG::getSuccs(SVFGNode *node) const {
  SVFGNodeSet result;
  if (!node)
    return result;

  std::queue<SVFGNode *> worklist;
  worklist.push(node);
  result.insert(node);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    for (auto *edge : current->getOutEdges()) {
      SVFGNode *succ = edge->getDstNode();
      if (result.insert(succ).second) {
        worklist.push(succ);
      }
    }
  }

  result.erase(node);
  return result;
}

bool SVFG::hasPath(SVFGNode *src, SVFGNode *dst) const {
  if (!src || !dst || src == dst)
    return src == dst;

  SVFGNodeSet visited;
  std::queue<SVFGNode *> worklist;
  worklist.push(src);
  visited.insert(src);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    if (current == dst)
      return true;

    for (auto *edge : current->getOutEdges()) {
      SVFGNode *succ = edge->getDstNode();
      if (visited.insert(succ).second) {
        worklist.push(succ);
      }
    }
  }

  return false;
}

void SVFG::dump(const std::string &filename) const {
  (void)SVFGSerializer::writeDot(*this, filename);
}

bool SVFG::writeToFile(const std::string &filename) const {
  return SVFGSerializer::writeText(*this, filename);
}

bool SVFG::readFromFile(const std::string &filename) {
  return SVFGSerializer::readText(*this, filename);
}

void SVFG::printStat() const {
  llvm::errs() << "=== SVFG Statistics ===\n";
  llvm::errs() << "Total Nodes: " << stat.numNodes << "\n";
  llvm::errs() << "  Addr Nodes: " << stat.numAddrNodes << "\n";
  llvm::errs() << "  Copy Nodes: " << stat.numCopyNodes << "\n";
  llvm::errs() << "  Load Nodes: " << stat.numLoadNodes << "\n";
  llvm::errs() << "  Store Nodes: " << stat.numStoreNodes << "\n";
  llvm::errs() << "  Gep Nodes: " << stat.numGepNodes << "\n";
  llvm::errs() << "  Phi Nodes: " << stat.numPhiNodes << "\n";
  llvm::errs() << "  Memory Nodes: " << stat.numMemNodes << "\n";
  llvm::errs() << "  Param Nodes: " << stat.numParamNodes << "\n";
  llvm::errs() << "Total Edges: " << stat.numEdges << "\n";
  llvm::errs() << "  Intra Edges: " << stat.numIntraEdges << "\n";
  llvm::errs() << "  Call Edges: " << stat.numCallEdges << "\n";
  llvm::errs() << "  Ret Edges: " << stat.numRetEdges << "\n";
}

void SVFG::swapWith(SVFG &other) {
  using std::swap;
  swap(nodeMap, other.nodeMap);
  swap(instToDefMap, other.instToDefMap);
  swap(valueToNodeMap, other.valueToNodeMap);
  swap(mssaVerToNodeMap, other.mssaVerToNodeMap);
  swap(callSiteToActualInMap, other.callSiteToActualInMap);
  swap(callSiteToActualOutMap, other.callSiteToActualOutMap);
  swap(callSiteToActualParmMap, other.callSiteToActualParmMap);
  swap(callSiteToActualRetMap, other.callSiteToActualRetMap);
  swap(funcToFormalInMap, other.funcToFormalInMap);
  swap(funcToFormalOutMap, other.funcToFormalOutMap);
  swap(funcToFormalParmMap, other.funcToFormalParmMap);
  swap(funcToFormalRetMap, other.funcToFormalRetMap);
  swap(icfg, other.icfg);
  swap(nextNodeId, other.nextNodeId);
  swap(stat, other.stat);
  swap(objectDebug, other.objectDebug);
  swap(nodeFunctionDebug, other.nodeFunctionDebug);
  swap(nodeCallSiteDebug, other.nodeCallSiteDebug);
  swap(nodesForUpdate, other.nodesForUpdate);
}

void SVFG::markForUpdate(SVFGNode *node) {
  if (node) {
    nodesForUpdate.insert(node);
  }
}

SVFGNodeSet SVFG::getNodesForUpdate() const {
  return nodesForUpdate;
}

void SVFG::clearUpdateMarkers() {
  nodesForUpdate.clear();
}
