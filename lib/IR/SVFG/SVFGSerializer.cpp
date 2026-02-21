#include "IR/SVFG/SVFGSerializer.h"

#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <vector>

#include <llvm/Support/raw_ostream.h>

using namespace lotus::analysis;
using namespace llvm;

static constexpr const char *kHeaderV2 = "SVFG-TEXT-V2";
static constexpr const char *kHeaderV3 = "SVFG-TEXT-V3";
static constexpr const char *kHeaderV4 = "SVFG-TEXT-V4";

bool SVFGSerializer::writeDot(const SVFG &graph, const std::string &filename) {
  std::ofstream file(filename);
  if (!file.is_open())
    return false;

  file << "digraph SVFG {\n";
  file << "  rankdir=TB;\n";
  file << "  node [shape=box, fontsize=10];\n";

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    file << "  N" << node->getId() << " [label=\"";
    file << "ID: " << node->getId() << "\\n";
    file << "Kind: " << static_cast<uint32_t>(node->getNodeKind());
    if (const auto *val = node->getValue()) {
      file << "\\nVal: " << val->getName().str();
    }
    file << "\"];\n";
  }

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    for (const SVFGEdge *edge : node->getOutEdges()) {
      file << "  N" << edge->getSrcNode()->getId() << " -> N"
           << edge->getDstNode()->getId();
      file << " [label=\"" << edge->toString();
      if (edge->hasCallSiteDebug()) {
        file << "\\n" << edge->getCallSiteDebug();
      }
      file << "\"";
      if (edge->hasCallSite() || edge->hasCallSiteDebug()) {
        file << ", color=darkgreen";
      }
      if (!edge->getPointsTo().empty()) {
        file << ", tooltip=\"pts=" << edge->getPointsTo().size() << "\"";
      }
      if (edge->getWeight() == SVFGEdge::EdgeWeight::Many) {
        file << ", style=dashed";
      }
      file << "];\n";
    }
  }

  file << "}\n";
  return true;
}

bool SVFGSerializer::writeText(const SVFG &graph, const std::string &filename) {
  std::ofstream file(filename);
  if (!file.is_open())
    return false;

  file << kHeaderV4 << "\n";

  // Persist object debug labels to preserve points-to identity across reloads.
  for (const auto &pair : graph.getObjectDebugMap()) {
    file << "O " << pair.first << " " << pair.second << "\n";
  }

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    std::string fnDebug = graph.getNodeFunctionDebug(node->getId());
    std::string csDebug = graph.getNodeCallSiteDebug(node->getId());
    if (fnDebug.empty()) {
      if (const llvm::Function *F = node->getFunction()) {
        fnDebug = F->getName().str();
      }
    }
    if (csDebug.empty()) {
      if (const auto *actualParm = dyn_cast<ActualParmSVFGNode>(node)) {
        if (actualParm->getCallSite()) {
          std::string label;
          llvm::raw_string_ostream os(label);
          os << actualParm->getCallSite()->getFunction()->getName() << "->";
          if (const llvm::Function *callee =
                  actualParm->getCallSite()->getCalledFunction()) {
            os << callee->getName();
          } else {
            os << "ind";
          }
          csDebug = os.str();
        }
      } else if (const auto *actualRet = dyn_cast<ActualRetSVFGNode>(node)) {
        if (actualRet->getCallSite()) {
          std::string label;
          llvm::raw_string_ostream os(label);
          os << actualRet->getCallSite()->getFunction()->getName() << "->";
          if (const llvm::Function *callee =
                  actualRet->getCallSite()->getCalledFunction()) {
            os << callee->getName();
          } else {
            os << "ind";
          }
          csDebug = os.str();
        }
      } else if (const auto *actualIn = dyn_cast<ActualInSVFGNode>(node)) {
        if (actualIn->getCallSite()) {
          std::string label;
          llvm::raw_string_ostream os(label);
          os << actualIn->getCallSite()->getFunction()->getName() << "->";
          if (const llvm::Function *callee =
                  actualIn->getCallSite()->getCalledFunction()) {
            os << callee->getName();
          } else {
            os << "ind";
          }
          csDebug = os.str();
        }
      } else if (const auto *actualOut = dyn_cast<ActualOutSVFGNode>(node)) {
        if (actualOut->getCallSite()) {
          std::string label;
          llvm::raw_string_ostream os(label);
          os << actualOut->getCallSite()->getFunction()->getName() << "->";
          if (const llvm::Function *callee =
                  actualOut->getCallSite()->getCalledFunction()) {
            os << callee->getName();
          } else {
            os << "ind";
          }
          csDebug = os.str();
        }
      } else if (const auto *callMu = dyn_cast<CallMuSVFGNode>(node)) {
        if (callMu->getCallSite()) {
          std::string label;
          llvm::raw_string_ostream os(label);
          os << callMu->getCallSite()->getFunction()->getName() << "->";
          if (const llvm::Function *callee = callMu->getCallSite()->getCalledFunction()) {
            os << callee->getName();
          } else {
            os << "ind";
          }
          csDebug = os.str();
        }
      } else if (const auto *callChi = dyn_cast<CallChiSVFGNode>(node)) {
        if (callChi->getCallSite()) {
          std::string label;
          llvm::raw_string_ostream os(label);
          os << callChi->getCallSite()->getFunction()->getName() << "->";
          if (const llvm::Function *callee = callChi->getCallSite()->getCalledFunction()) {
            os << callee->getName();
          } else {
            os << "ind";
          }
          csDebug = os.str();
        }
      }
    }
    if (!fnDebug.empty() || !csDebug.empty()) {
      file << "M " << node->getId() << " " << std::quoted(fnDebug) << " "
           << std::quoted(csDebug) << "\n";
    }
  }

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    uint32_t aux0 = 0;
    uint32_t aux1 = 0;
    switch (node->getNodeKind()) {
    case SVFGK::Load:
      aux0 = dynamic_cast<const LoadSVFGNode *>(node)->getLoadFromPtr();
      break;
    case SVFGK::Store:
      aux0 = dynamic_cast<const StoreSVFGNode *>(node)->getStoreToPtr();
      break;
    case SVFGK::FormalParm:
      aux0 = dynamic_cast<const FormalParmSVFGNode *>(node)->getParamIndex();
      break;
    case SVFGK::ActualParm:
      aux0 = dynamic_cast<const ActualParmSVFGNode *>(node)->getParamIndex();
      break;
    default:
      break;
    }

    SVFGNodeBS pts;
    if (const auto *p = node->getPointsTo()) {
      pts = *p;
    }

    file << "N " << node->getId() << " "
         << static_cast<uint32_t>(node->getNodeKind()) << " "
         << node->getMemReg() << " " << node->getSSAVersion() << " " << aux0
         << " " << aux1 << " " << pts.size();
    for (uint32_t pt : pts) {
      file << " " << pt;
    }
    file << "\n";
  }

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    for (const SVFGEdge *edge : node->getOutEdges()) {
      file << "E " << edge->getSrcNode()->getId() << " "
           << edge->getDstNode()->getId() << " "
           << static_cast<uint32_t>(edge->getEdgeKind()) << " "
           << static_cast<uint32_t>(edge->getWeight()) << " "
           << edge->getPointsTo().size();
      for (uint32_t pt : edge->getPointsTo()) {
        file << " " << pt;
      }
      const std::string &cs = edge->getCallSiteDebug();
      if (!cs.empty()) {
        file << " " << std::quoted(cs);
      }
      file << "\n";
    }
  }
  return true;
}

static SVFGNode *createNodeForKind(uint32_t id, SVFGK kind, uint32_t memReg,
                                   uint32_t version, uint32_t aux0,
                                   const SVFGNodeBS &pts) {
  const ICFGNode *icfg = nullptr;
  switch (kind) {
  case SVFGK::Stmt:
    return new StmtSVFGNode(id, SVFGK::Stmt, icfg, nullptr);
  case SVFGK::Addr:
    return new AddrSVFGNode(id, icfg, nullptr);
  case SVFGK::Copy:
    return new CopySVFGNode(id, icfg, nullptr);
  case SVFGK::Load:
    return new LoadSVFGNode(id, icfg, nullptr, aux0);
  case SVFGK::Store:
    return new StoreSVFGNode(id, icfg, nullptr, aux0);
  case SVFGK::Gep:
    return new GepSVFGNode(id, icfg, nullptr);
  case SVFGK::BinaryOp:
    return new BinaryOpSVFGNode(id, icfg, nullptr);
  case SVFGK::UnaryOp:
    return new UnaryOpSVFGNode(id, icfg, nullptr);
  case SVFGK::Cmp:
    return new CmpSVFGNode(id, icfg, nullptr);
  case SVFGK::Branch:
    return new BranchSVFGNode(id, icfg, nullptr);
  case SVFGK::Phi:
    return new PhiSVFGNode(id, SVFGK::Phi, icfg, nullptr);
  case SVFGK::IntraPhi:
    return new IntraPhiSVFGNode(id, icfg, nullptr);
  case SVFGK::InterPhi:
    return new InterPhiSVFGNode(id, icfg,
                                static_cast<const llvm::Function *>(nullptr));
  case SVFGK::MPhi:
    return new MSSAPhiSVFGNode(id, SVFGK::MPhi, icfg, memReg, pts);
  case SVFGK::MIntraPhi:
    return new IntraMSSAPhiSVFGNode(id, icfg, memReg, version, pts);
  case SVFGK::MInterPhi:
    return new InterMSSAPhiSVFGNode(
        id, icfg, static_cast<const llvm::Function *>(nullptr), memReg, pts);
  case SVFGK::FormalIn:
    return new FormalInSVFGNode(id, icfg, nullptr, memReg, pts);
  case SVFGK::FormalOut:
    return new FormalOutSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::ActualIn:
    return new ActualInSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::ActualOut:
    return new ActualOutSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::LoadMu:
    return new LoadMuSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::StoreChi:
    return new StoreChiSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::CallMu:
    return new CallMuSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::CallChi:
    return new CallChiSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::RetMu:
    return new RetMuSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::EntryChi:
    return new EntryChiSVFGNode(id, icfg, nullptr, memReg, pts, version);
  case SVFGK::FormalParm:
    return new FormalParmSVFGNode(id, icfg, nullptr, aux0);
  case SVFGK::ActualParm:
    return new ActualParmSVFGNode(id, icfg, nullptr, aux0);
  case SVFGK::FormalRet:
    return new FormalRetSVFGNode(id, icfg, nullptr);
  case SVFGK::ActualRet:
    return new ActualRetSVFGNode(id, icfg, nullptr);
  case SVFGK::VarArg:
    return new VarArgSVFGNode(id, icfg, nullptr);
  case SVFGK::NullPtr:
    return new NullPtrSVFGNode(id, icfg);
  case SVFGK::Dummy:
    return new DummySVFGNode(id, icfg);
  case SVFGK::DummyVProp:
    return new DummyVersionPropSVFGNode(id, icfg, memReg, version);
  default:
    return new DummySVFGNode(id, icfg);
  }
}

bool SVFGSerializer::readText(SVFG &graph, const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open())
    return false;

  struct SerializedEdge {
    uint32_t src = 0;
    uint32_t dst = 0;
    uint32_t kind = 0;
    uint32_t weight = static_cast<uint32_t>(SVFGEdge::EdgeWeight::One);
    SVFGNodeBS pts;
    std::string callSiteDebug;
  };
  std::vector<SerializedEdge> edges;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    if (line == kHeaderV2) {
      continue;
    }
    if (line == kHeaderV3) {
      continue;
    }
    if (line == kHeaderV4) {
      continue;
    }
    std::istringstream iss(line);
    char tag = 0;
    iss >> tag;
    if (tag == 'O') {
      uint32_t objId = 0;
      iss >> objId;
      std::string label;
      std::getline(iss, label);
      // Trim leading space.
      if (!label.empty() && label.front() == ' ')
        label.erase(label.begin());
      graph.setObjectDebug(objId, std::move(label));
      continue;
    }
    if (tag == 'M') {
      uint32_t nodeId = 0;
      std::string fnDebug;
      std::string csDebug;
      iss >> nodeId >> std::quoted(fnDebug) >> std::quoted(csDebug);
      if (!fnDebug.empty()) {
        graph.setNodeFunctionDebug(nodeId, fnDebug);
      }
      if (!csDebug.empty()) {
        graph.setNodeCallSiteDebug(nodeId, csDebug);
      }
      continue;
    }
    if (tag == 'N') {
      uint32_t id = 0;
      uint32_t kindVal = 0;
      uint32_t memReg = 0;
      uint32_t version = 0;
      uint32_t aux0 = 0;
      uint32_t aux1 = 0;
      uint32_t ptsCount = 0;
      iss >> id >> kindVal >> memReg >> version;

      // V2 adds aux0 aux1 ptsCount pts...
      if (iss >> aux0 >> aux1 >> ptsCount) {
        // ok
      } else {
        // V1 format: "N id kind memReg version"
        aux0 = 0;
        aux1 = 0;
        ptsCount = 0;
        iss.clear();
      }
      (void)aux1;
      SVFGNodeBS pts;
      for (uint32_t i = 0; i < ptsCount; ++i) {
        uint32_t pt = 0;
        if (!(iss >> pt))
          break;
        pts.insert(pt);
      }
      SVFGNode *node = createNodeForKind(id, static_cast<SVFGK>(kindVal),
                                         memReg, version, aux0, pts);
      graph.addNode(node);
    } else if (tag == 'E') {
      SerializedEdge edge;
      iss >> edge.src >> edge.dst >> edge.kind;

      // Optional fields for newer format.
      if (iss >> edge.weight) {
        uint32_t ptsCount = 0;
        if (iss >> ptsCount) {
          for (uint32_t i = 0; i < ptsCount; ++i) {
            uint32_t pt = 0;
            if (!(iss >> pt))
              break;
            edge.pts.insert(pt);
          }
          std::string callSiteDebug;
          if (iss >> std::quoted(callSiteDebug)) {
            edge.callSiteDebug = std::move(callSiteDebug);
          } else {
            iss.clear();
            std::string fallback;
            if (iss >> fallback) {
              edge.callSiteDebug = std::move(fallback);
            }
          }
        }
      }

      edges.push_back(std::move(edge));
    }
  }

  for (const auto &edgeInfo : edges) {
    SVFGNode *src = graph.getNode(edgeInfo.src);
    SVFGNode *dst = graph.getNode(edgeInfo.dst);
    if (src && dst) {
      SVFGEdge *edge =
          graph.addEdge(src, dst, static_cast<SVFGEdgeK>(edgeInfo.kind),
                        nullptr, edgeInfo.pts, edgeInfo.callSiteDebug);
      if (edge) {
        edge->setWeight(static_cast<SVFGEdge::EdgeWeight>(edgeInfo.weight));
      }
    }
  }
  return true;
}
