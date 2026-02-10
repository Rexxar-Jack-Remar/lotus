//===- SVFGEdge.h -- SVFG Edge Definitions
//------------------------------------//
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
//
// SVFGEdge: Complete edge type hierarchy for Sparse Value-Flow Graph.
//
// This file provides a comprehensive hierarchy of edge types mirroring SVF's
// design:
// - Intra-procedural edges: Copy, Load, Store, Gep, Phi, etc.
// - Call edges: Direct/indirect parameter passing
// - Return edges: Direct/indirect return value flow
// - Memory edges: MU/CHI for memory SSA
//
// Key design features:
// - Comprehensive edge classification
// - Edge weight for cost-sensitive analysis
// - Pointer to edge sets for efficient lookups
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/SVFG/SVFGBase.h"

#include <llvm/IR/InstrTypes.h>
#include <set>
#include <string>
#include <utility>

namespace lotus {
namespace analysis {

class SVFGNode;

/// @brief Value-flow edge connecting SVFG nodes
class SVFGEdge {
public:
  /// @brief Edge weight for analysis cost estimation
  enum class EdgeWeight : uint8_t {
    Zero = 0, // No cost (e.g., epsilon transitions)
    One = 1,  // Standard weight
    Many      // Unbounded (for loops, recursion)
  };

private:
  SVFGNode *src;
  SVFGNode *dst;
  SVFGEdgeK kind;
  EdgeWeight weight;
  const llvm::CallBase *callSite;
  std::set<uint32_t> pointsTo;

public:
  /// @brief Construct edge
  SVFGEdge(SVFGNode *s, SVFGNode *d, SVFGEdgeK k,
           EdgeWeight w = EdgeWeight::One,
           const llvm::CallBase *cs = nullptr,
           std::set<uint32_t> pts = {})
      : src(s), dst(d), kind(k), weight(w), callSite(cs),
        pointsTo(std::move(pts)) {}

  /// @brief Destructor
  virtual ~SVFGEdge() = default;

  //===------------------------------------------------------------------===
  // Accessors
  //===------------------------------------------------------------------===

  inline SVFGNode *getSrcNode() const { return src; }
  inline SVFGNode *getDstNode() const { return dst; }
  inline SVFGEdgeK getEdgeKind() const { return kind; }
  inline EdgeWeight getWeight() const { return weight; }
  inline void setWeight(EdgeWeight w) { weight = w; }
  inline const llvm::CallBase *getCallSite() const { return callSite; }
  inline bool hasCallSite() const { return callSite != nullptr; }
  inline const std::set<uint32_t> &getPointsTo() const { return pointsTo; }
  inline bool addPointsTo(const std::set<uint32_t> &pts) {
    const size_t oldSize = pointsTo.size();
    pointsTo.insert(pts.begin(), pts.end());
    return pointsTo.size() != oldSize;
  }

  //===------------------------------------------------------------------===
  // Classification
  //===------------------------------------------------------------------===

  inline bool isIntraEdge() const { return isIntraVFGEdge(kind); }
  inline bool isCallEdge() const { return isCallVFGEdge(kind); }
  inline bool isRetEdge() const { return isRetVFGEdge(kind); }
  inline bool isMemoryEdge() const { return isMemVFGEdge(kind); }
  inline bool isCopyEdge() const { return kind == SVFGEdgeK::IntraCopy; }
  inline bool isLoadEdge() const { return kind == SVFGEdgeK::IntraLoad; }
  inline bool isStoreEdge() const { return kind == SVFGEdgeK::IntraStore; }
  inline bool isPhiEdge() const { return kind == SVFGEdgeK::IntraPhi; }

  //===------------------------------------------------------------------===
  // Utility
  //===------------------------------------------------------------------===

  std::string toString() const;
};

/// @brief Intra-procedural copy edge
class IntraCopyVFGEdge : public SVFGEdge {
public:
  IntraCopyVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraCopy) {}
  SVFG_EDGE_KIND(IntraCopy)
};

/// @brief Intra-procedural load edge
class IntraLoadVFGEdge : public SVFGEdge {
public:
  IntraLoadVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraLoad) {}
  SVFG_EDGE_KIND(IntraLoad)
};

/// @brief Intra-procedural store edge
class IntraStoreVFGEdge : public SVFGEdge {
public:
  IntraStoreVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraStore) {}
  SVFG_EDGE_KIND(IntraStore)
};

/// @brief Intra-procedural GEP edge
class IntraGepVFGEdge : public SVFGEdge {
public:
  IntraGepVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraGep) {}
  SVFG_EDGE_KIND(IntraGep)
};

/// @brief Intra-procedural PHI edge
class IntraPhiVFGEdge : public SVFGEdge {
public:
  IntraPhiVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraPhi) {}
  SVFG_EDGE_KIND(IntraPhi)
};

/// @brief Memory use edge (load)
class IntraMuVFGEdge : public SVFGEdge {
public:
  IntraMuVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraMu) {}
  SVFG_EDGE_KIND(IntraMu)
};

/// @brief Memory def edge (store)
class IntraChiVFGEdge : public SVFGEdge {
public:
  IntraChiVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraChi) {}
  SVFG_EDGE_KIND(IntraChi)
};

/// @brief Direct call edge
class CallDirVFGEdge : public SVFGEdge {
public:
  CallDirVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::CallDir) {}
  SVFG_EDGE_KIND(CallDir)
};

/// @brief Indirect call edge
class CallIndVFGEdge : public SVFGEdge {
public:
  CallIndVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::CallInd) {}
  SVFG_EDGE_KIND(CallInd)
};

/// @brief Call actual-in to formal-in edge
class CallAInVFGEdge : public SVFGEdge {
public:
  CallAInVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::CallAIn) {}
  SVFG_EDGE_KIND(CallAIn)
};

/// @brief Call formal-in edge
class CallFInVFGEdge : public SVFGEdge {
public:
  CallFInVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::CallFIn) {}
  SVFG_EDGE_KIND(CallFIn)
};

/// @brief Parameter call edge
class ParamCallVFGEdge : public SVFGEdge {
public:
  ParamCallVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::ParamCall) {}
  SVFG_EDGE_KIND(ParamCall)
};

/// @brief Direct return edge
class RetDirVFGEdge : public SVFGEdge {
public:
  RetDirVFGEdge(SVFGNode *s, SVFGNode *d) : SVFGEdge(s, d, SVFGEdgeK::RetDir) {}
  SVFG_EDGE_KIND(RetDir)
};

/// @brief Indirect return edge
class RetIndVFGEdge : public SVFGEdge {
public:
  RetIndVFGEdge(SVFGNode *s, SVFGNode *d) : SVFGEdge(s, d, SVFGEdgeK::RetInd) {}
  SVFG_EDGE_KIND(RetInd)
};

/// @brief Return actual-out to formal-out edge
class RetAOutVFGEdge : public SVFGEdge {
public:
  RetAOutVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::RetAOut) {}
  SVFG_EDGE_KIND(RetAOut)
};

/// @brief Return formal-out edge
class RetFOutVFGEdge : public SVFGEdge {
public:
  RetFOutVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::RetFOut) {}
  SVFG_EDGE_KIND(RetFOut)
};

/// @brief Parameter return edge
class ParamRetVFGEdge : public SVFGEdge {
public:
  ParamRetVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::ParamRet) {}
  SVFG_EDGE_KIND(ParamRet)
};

} // namespace analysis
} // namespace lotus
