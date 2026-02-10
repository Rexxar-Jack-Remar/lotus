#include "IR/SVFG/SVFGSerializer.h"

#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <fstream>
#include <sstream>
#include <tuple>
#include <vector>

using namespace lotus::analysis;

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
      file << " [label=\"" << edge->toString() << "\"";
      if (edge->hasCallSite()) {
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

  for (const auto &pair : graph) {
    const SVFGNode *node = pair.second;
    file << "N " << node->getId() << " " << static_cast<uint32_t>(node->getNodeKind())
         << " " << node->getMemReg() << " " << node->getSSAVersion() << "\n";
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
      file << "\n";
    }
  }
  return true;
}

static SVFGNode *createNodeForKind(uint32_t id, SVFGK kind, uint32_t memReg,
                                   uint32_t version) {
  const ICFGNode *icfg = nullptr;
  switch (kind) {
  case SVFGK::Addr:
    return new AddrSVFGNode(id, icfg, nullptr);
  case SVFGK::Copy:
    return new CopySVFGNode(id, icfg, nullptr);
  case SVFGK::Load:
    return new LoadSVFGNode(id, icfg, nullptr, 0);
  case SVFGK::Store:
    return new StoreSVFGNode(id, icfg, nullptr, 0);
  case SVFGK::Gep:
    return new GepSVFGNode(id, icfg, nullptr);
  case SVFGK::BinaryOp:
    return new BinaryOpSVFGNode(id, icfg, nullptr);
  case SVFGK::Cmp:
    return new CmpSVFGNode(id, icfg, nullptr);
  case SVFGK::Branch:
    return new BranchSVFGNode(id, icfg, nullptr);
  case SVFGK::IntraPhi:
    return new IntraPhiSVFGNode(id, icfg, nullptr);
  case SVFGK::InterPhi:
    return new InterPhiSVFGNode(id, icfg,
                                static_cast<const llvm::Function *>(nullptr));
  case SVFGK::MIntraPhi:
    return new IntraMSSAPhiSVFGNode(id, icfg, memReg, version, {});
  case SVFGK::MInterPhi:
    return new InterMSSAPhiSVFGNode(id, icfg,
                                    static_cast<const llvm::Function *>(nullptr),
                                    memReg, {});
  case SVFGK::FormalIn:
    return new FormalInSVFGNode(id, icfg, nullptr, memReg, {});
  case SVFGK::FormalOut:
    return new FormalOutSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::ActualIn:
    return new ActualInSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::ActualOut:
    return new ActualOutSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::LoadMu:
    return new LoadMuSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::StoreChi:
    return new StoreChiSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::CallMu:
    return new CallMuSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::CallChi:
    return new CallChiSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::RetMu:
    return new RetMuSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::EntryChi:
    return new EntryChiSVFGNode(id, icfg, nullptr, memReg, {}, version);
  case SVFGK::FormalParm:
    return new FormalParmSVFGNode(id, icfg, nullptr, 0);
  case SVFGK::ActualParm:
    return new ActualParmSVFGNode(id, icfg, nullptr, 0);
  case SVFGK::FormalRet:
    return new FormalRetSVFGNode(id, icfg, nullptr);
  case SVFGK::ActualRet:
    return new ActualRetSVFGNode(id, icfg, nullptr);
  case SVFGK::NullPtr:
    return new NullPtrSVFGNode(id, icfg);
  case SVFGK::Dummy:
    return new DummySVFGNode(id, icfg);
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
  };
  std::vector<SerializedEdge> edges;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    std::istringstream iss(line);
    char tag = 0;
    iss >> tag;
    if (tag == 'N') {
      uint32_t id = 0;
      uint32_t kindVal = 0;
      uint32_t memReg = 0;
      uint32_t version = 0;
      iss >> id >> kindVal >> memReg >> version;
      SVFGNode *node =
          createNodeForKind(id, static_cast<SVFGK>(kindVal), memReg, version);
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
        }
      }

      edges.push_back(std::move(edge));
    }
  }

  for (const auto &edgeInfo : edges) {
    SVFGNode *src = graph.getNode(edgeInfo.src);
    SVFGNode *dst = graph.getNode(edgeInfo.dst);
    if (src && dst) {
      SVFGEdge *edge = graph.addEdge(src, dst, static_cast<SVFGEdgeK>(edgeInfo.kind),
                                     nullptr, edgeInfo.pts);
      if (edge) {
        edge->setWeight(static_cast<SVFGEdge::EdgeWeight>(edgeInfo.weight));
      }
    }
  }
  return true;
}
