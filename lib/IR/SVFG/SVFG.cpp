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
  updateStat(node);
}

SVFGEdge *SVFG::addEdge(SVFGNode *src, SVFGNode *dst, SVFGEdgeK kind,
                        const llvm::CallBase *callSite,
                        const SVFGNodeBS &pointsTo) {
  if (!src || !dst)
    return nullptr;

  // Check for duplicate edge
  for (auto *existing : src->getOutEdges()) {
    if (existing->getDstNode() == dst && existing->getEdgeKind() == kind &&
        existing->getCallSite() == callSite) {
      existing->addPointsTo(pointsTo);
      return existing;
    }
  }

  SVFGEdge *edge = new SVFGEdge(src, dst, kind, SVFGEdge::EdgeWeight::One,
                                callSite, pointsTo);
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
  swap(funcToFormalInMap, other.funcToFormalInMap);
  swap(funcToFormalOutMap, other.funcToFormalOutMap);
  swap(icfg, other.icfg);
  swap(nextNodeId, other.nextNodeId);
  swap(stat, other.stat);
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
