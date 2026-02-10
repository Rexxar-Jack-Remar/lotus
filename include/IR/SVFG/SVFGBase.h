//===- SVFGBase.h -- SVFG Base Infrastructure --------------------------------//
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
// SVFG Base: Core types and infrastructure for Sparse Value-Flow Graph.
//
// This file defines the fundamental types used throughout SVFG, including:
// - Node and edge kinds (SVFGK)
// - Type-safe casting support (isa/cast/dyn_cast)
// - Base graph infrastructure
//
// Design principles aligned with SVF:
// - Conservative memory modeling
// - Support for both top-level and address-taken variables
// - Memory SSA form for precise alias tracking
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <set>
#include <string>

namespace lotus {
namespace analysis {

// Forward declarations
class SVFGNode;
class SVFGEdge;

/// @brief Node kinds for SVFG (aligned with SVF's VFGNodeK)
enum class SVFGK : uint32_t {
  // Statement nodes (top-level pointers)
  Stmt = 0,
  Addr,
  Copy,
  Load,
  Store,
  Gep,
  BinaryOp,
  Cmp,
  Branch,

  // PHI nodes
  Phi,
  IntraPhi,
  InterPhi,

  // Memory SSA nodes (address-taken variables)
  MPhi,
  MIntraPhi,
  MInterPhi,

  // Parameter nodes
  FormalParm,
  ActualParm,
  FormalRet,
  ActualRet,

  // Memory SSA entry/exit
  FormalIn,
  FormalOut,
  ActualIn,
  ActualOut,

  // Memory SSA use/def
  LoadMu,
  StoreChi,
  CallMu,
  CallChi,
  RetMu,
  EntryChi,

  // Null and dummy nodes
  NullPtr,
  Dummy,

  // Variant kinds
  Variant,

  // Total count (for sizing arrays)
  Total
};

/// @brief Edge kinds for SVFG (aligned with SVF's VFGEdgeK)
enum class SVFGEdgeK : uint32_t {
  // Intra-procedural edges
  IntraDirect = 0,
  IntraCopy,
  IntraLoad,
  IntraStore,
  IntraGep,
  IntraCmp,
  IntraBranch,
  IntraPhi,
  IntraMu,
  IntraChi,

  // Memory SSA edges
  CallMu,
  CallChi,
  RetMu,
  EntryChi,

  // Inter-procedural call edges
  CallDir,
  CallInd,
  CallAIn,
  CallFIn,

  // Inter-procedural return edges
  RetDir,
  RetInd,
  RetAOut,
  RetFOut,

  // Parameter passing edges
  ParamCall,
  ParamRet,

  // Variant edges
  Variant,

  // Total count
  Total
};

/// @brief Check if node kind is a statement node
inline bool isStmtSVFGNode(SVFGK k) {
  return k == SVFGK::Stmt || k == SVFGK::Addr || k == SVFGK::Copy ||
         k == SVFGK::Load || k == SVFGK::Store || k == SVFGK::Gep ||
         k == SVFGK::BinaryOp || k == SVFGK::Cmp || k == SVFGK::Branch;
}

/// @brief Check if node kind is a memory SSA node
inline bool isMemSVFGNode(SVFGK k) {
  return k == SVFGK::FormalIn || k == SVFGK::FormalOut ||
         k == SVFGK::ActualIn || k == SVFGK::ActualOut || k == SVFGK::MPhi ||
         k == SVFGK::MIntraPhi || k == SVFGK::MInterPhi || k == SVFGK::LoadMu ||
         k == SVFGK::StoreChi || k == SVFGK::CallMu || k == SVFGK::CallChi ||
         k == SVFGK::RetMu || k == SVFGK::EntryChi;
}

/// @brief Check if node kind is a PHI node
inline bool isPhiSVFGNode(SVFGK k) {
  return k == SVFGK::Phi || k == SVFGK::IntraPhi || k == SVFGK::InterPhi;
}

/// @brief Check if node kind is an inter-procedural PHI
inline bool isInterPhiSVFGNode(SVFGK k) { return k == SVFGK::InterPhi; }

/// @brief Check if node kind is an intra-procedural PHI
inline bool isIntraPhiSVFGNode(SVFGK k) { return k == SVFGK::IntraPhi; }

/// @brief Check if node kind is a memory PHI
inline bool isMPhiSVFGNode(SVFGK k) {
  return k == SVFGK::MPhi || k == SVFGK::MIntraPhi || k == SVFGK::MInterPhi;
}

/// @brief Check if node kind is a parameter node
inline bool isParamSVFGNode(SVFGK k) {
  return k == SVFGK::FormalParm || k == SVFGK::ActualParm ||
         k == SVFGK::FormalRet || k == SVFGK::ActualRet;
}

/// @brief Check if edge kind is intra-procedural
inline bool isIntraVFGEdge(SVFGEdgeK k) {
  return k == SVFGEdgeK::IntraDirect || k == SVFGEdgeK::IntraCopy ||
         k == SVFGEdgeK::IntraLoad || k == SVFGEdgeK::IntraStore ||
         k == SVFGEdgeK::IntraGep || k == SVFGEdgeK::IntraCmp ||
         k == SVFGEdgeK::IntraBranch || k == SVFGEdgeK::IntraPhi ||
         k == SVFGEdgeK::IntraMu || k == SVFGEdgeK::IntraChi;
}

/// @brief Check if edge kind is a call edge
inline bool isCallVFGEdge(SVFGEdgeK k) {
  return k == SVFGEdgeK::CallDir || k == SVFGEdgeK::CallInd ||
         k == SVFGEdgeK::CallAIn || k == SVFGEdgeK::CallFIn ||
         k == SVFGEdgeK::ParamCall;
}

/// @brief Check if edge kind is a return edge
inline bool isRetVFGEdge(SVFGEdgeK k) {
  return k == SVFGEdgeK::RetDir || k == SVFGEdgeK::RetInd ||
         k == SVFGEdgeK::RetAOut || k == SVFGEdgeK::RetFOut ||
         k == SVFGEdgeK::ParamRet;
}

/// @brief Check if edge kind is a memory edge
inline bool isMemVFGEdge(SVFGEdgeK k) {
  return k == SVFGEdgeK::IntraMu || k == SVFGEdgeK::IntraChi ||
         k == SVFGEdgeK::CallMu || k == SVFGEdgeK::CallChi ||
         k == SVFGEdgeK::RetMu || k == SVFGEdgeK::EntryChi ||
         k == SVFGEdgeK::CallAIn || k == SVFGEdgeK::CallFIn ||
         k == SVFGEdgeK::RetAOut || k == SVFGEdgeK::RetFOut;
}

} // namespace analysis
} // namespace lotus

// LLVM-style RTTI helpers
#define SVFG_NODE_KIND(K)                                                      \
  static inline bool classof(const lotus::analysis::SVFGNode *n) {             \
    return n->getNodeKind() == SVFGK::K;                                       \
  }

#define SVFG_EDGE_KIND(K)                                                      \
  static inline bool classof(const lotus::analysis::SVFGEdge *e) {             \
    return e->getEdgeKind() == SVFGEdgeK::K;                                   \
  }
