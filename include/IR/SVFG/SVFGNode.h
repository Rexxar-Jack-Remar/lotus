//===- SVFGNode.h -- SVFG Node Definitions -----------------------------------//
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
// SVFGNode: Complete node type hierarchy for Sparse Value-Flow Graph.
//
// This file provides a comprehensive hierarchy of node types mirroring SVF's design:
// - Statement nodes: Addr, Copy, Load, Store, Gep, BinaryOp, Cmp, Branch
// - PHI nodes: Phi, IntraPhi, InterPhi (for both values and memory)
// - Memory SSA nodes: FormalIn/Out, ActualIn/Out, LoadMu, StoreChi, etc.
// - Parameter nodes: FormalParm, ActualParm, FormalRet, ActualRet
//
// Key design features:
// - Type-safe casting via LLVM-style RTTI
// - Points-to set tracking for memory nodes
// - Full ICFG and call site integration
// - Memory SSA version support
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/SVFG/SVFGBase.h"
#include "IR/ICFG/ICFGNode.h"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <set>
#include <vector>

namespace lotus {
namespace analysis {

// Forward declarations
class SVFGEdge;

/// @brief NodeBS: Points-to set using lotus's AndersPtsSet style
using SVFGNodeBS = std::set<uint32_t>;

/// @brief Base class for all SVFG nodes
class SVFGNode
{
private:
    uint32_t id;
    SVFGK kind;
    
protected:
    const ICFGNode* icfgNode;
    std::vector<SVFGEdge*> inEdges;
    std::vector<SVFGEdge*> outEdges;
    
public:
    /// @brief Construct base node
    SVFGNode(uint32_t nodeId, SVFGK k) : id(nodeId), kind(k), icfgNode(nullptr) {}
    
    /// @brief Construct node with ICFG association
    SVFGNode(uint32_t nodeId, SVFGK k, const ICFGNode* icfg) 
        : id(nodeId), kind(k), icfgNode(icfg) {}
    
    /// @brief Destructor
    virtual ~SVFGNode() = default;
    
    //===------------------------------------------------------------------===
    // Accessors
    //===------------------------------------------------------------------===
    
    inline uint32_t getId() const { return id; }
    inline SVFGK getNodeKind() const { return kind; }
    inline const ICFGNode* getICFGNode() const { return icfgNode; }
    inline void setICFGNode(const ICFGNode* icfg) { icfgNode = icfg; }
    
    //===------------------------------------------------------------------===
    // Edge access
    //===------------------------------------------------------------------===
    
    inline const std::vector<SVFGEdge*>& getInEdges() const { return inEdges; }
    inline const std::vector<SVFGEdge*>& getOutEdges() const { return outEdges; }
    inline void addInEdge(SVFGEdge* e) { inEdges.push_back(e); }
    inline void addOutEdge(SVFGEdge* e) { outEdges.push_back(e); }
    inline void removeInEdge(SVFGEdge* e) {
      inEdges.erase(std::remove(inEdges.begin(), inEdges.end(), e), inEdges.end());
    }
    inline void removeOutEdge(SVFGEdge* e) {
      outEdges.erase(std::remove(outEdges.begin(), outEdges.end(), e), outEdges.end());
    }
    
    //===------------------------------------------------------------------===
    // Virtual interface (to be overridden)
    //===------------------------------------------------------------------===
    
    /// @brief Get points-to set (for memory nodes)
    virtual const SVFGNodeBS* getPointsTo() const { return nullptr; }
    
    /// @brief Get LLVM value for debugging
    virtual const llvm::Value* getValue() const { return nullptr; }
    
    /// @brief Get LLVM instruction
    virtual const llvm::Instruction* getInstruction() const { return nullptr; }
    
    /// @brief Get function containing this node
    virtual const llvm::Function* getFunction() const;
    
    /// @brief Get memory region (for memory SSA nodes)
    virtual uint32_t getMemReg() const { return 0; }
    
    /// @brief Get SSA version (for memory SSA nodes)
    virtual uint32_t getSSAVersion() const { return 0; }
    
    //===------------------------------------------------------------------===
    // Type inquiry
    //===------------------------------------------------------------------===
    
    inline bool isStmtNode() const { return isStmtSVFGNode(kind); }
    inline bool isMemNode() const { return isMemSVFGNode(kind); }
    inline bool isPhiNode() const { return isPhiSVFGNode(kind); }
    inline bool isParamNode() const { return isParamSVFGNode(kind); }
    inline bool isIntraPhiNode() const { return isIntraPhiSVFGNode(kind); }
    inline bool isInterPhiNode() const { return isInterPhiSVFGNode(kind); }
    inline bool isAddrTakenNode() const { return isMemSVFGNode(kind); }
    virtual bool isNullPtrNode() const { return false; }
    virtual bool isDummyNode() const { return false; }
    
    //===------------------------------------------------------------------===
    // Value-flow queries
    //===------------------------------------------------------------------===
    
    /// @brief Get defined SVF variables (for analysis clients)
    virtual SVFGNodeBS getDefSVFVars() const {
        if (const auto* pts = getPointsTo()) {
            return *pts;
        }
        return SVFGNodeBS();
    }
    
    /// @brief Get node as string for debugging
    virtual std::string toString() const;
};

/// @brief Statement node base (top-level pointer operations)
class StmtSVFGNode : public SVFGNode
{
protected:
    const llvm::Value* value;
    
public:
    StmtSVFGNode(uint32_t id, SVFGK k, const ICFGNode* icfg, const llvm::Value* v)
        : SVFGNode(id, k, icfg), value(v) {}
    
    const llvm::Value* getValue() const override { return value; }
    const llvm::Instruction* getInstruction() const override {
        return llvm::dyn_cast<llvm::Instruction>(value);
    }
    
    SVFG_NODE_KIND(Stmt)
};

/// @brief Address-taking instruction (alloca, malloc, address-of)
class AddrSVFGNode : public StmtSVFGNode
{
public:
    AddrSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Value* v)
        : StmtSVFGNode(id, SVFGK::Addr, icfg, v) {}
    
    SVFG_NODE_KIND(Addr)
};

/// @brief Copy instruction (mov, bitcast, inttoptr, etc.)
class CopySVFGNode : public StmtSVFGNode
{
public:
    CopySVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Value* v)
        : StmtSVFGNode(id, SVFGK::Copy, icfg, v) {}
    
    SVFG_NODE_KIND(Copy)
};

/// @brief Load instruction (memory read)
class LoadSVFGNode : public StmtSVFGNode
{
private:
    uint32_t loadFrom;  // Pointer being loaded from
    
public:
    LoadSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Value* v, uint32_t ptr)
        : StmtSVFGNode(id, SVFGK::Load, icfg, v), loadFrom(ptr) {}
    
    inline uint32_t getLoadFromPtr() const { return loadFrom; }
    
    SVFG_NODE_KIND(Load)
};

/// @brief Store instruction (memory write)
class StoreSVFGNode : public StmtSVFGNode
{
private:
    uint32_t storeTo;  // Pointer being stored to
    
public:
    StoreSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Value* v, uint32_t ptr)
        : StmtSVFGNode(id, SVFGK::Store, icfg, v), storeTo(ptr) {}
    
    inline uint32_t getStoreToPtr() const { return storeTo; }
    
    SVFG_NODE_KIND(Store)
};

/// @brief GetElementPtr instruction
class GepSVFGNode : public StmtSVFGNode
{
public:
    GepSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Value* v)
        : StmtSVFGNode(id, SVFGK::Gep, icfg, v) {}
    
    SVFG_NODE_KIND(Gep)
};

/// @brief Binary operation node
class BinaryOpSVFGNode : public StmtSVFGNode
{
public:
    BinaryOpSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Value* v)
        : StmtSVFGNode(id, SVFGK::BinaryOp, icfg, v) {}
    
    SVFG_NODE_KIND(BinaryOp)
};

/// @brief Comparison instruction node
class CmpSVFGNode : public StmtSVFGNode
{
public:
    CmpSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Value* v)
        : StmtSVFGNode(id, SVFGK::Cmp, icfg, v) {}
    
    SVFG_NODE_KIND(Cmp)
};

/// @brief Branch instruction node
class BranchSVFGNode : public StmtSVFGNode
{
public:
    BranchSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Value* v)
        : StmtSVFGNode(id, SVFGK::Branch, icfg, v) {}
    
    SVFG_NODE_KIND(Branch)
};

/// @brief Base PHI node (SSA form)
class PhiSVFGNode : public SVFGNode
{
protected:
    const llvm::PHINode* phiNode;
    
public:
    PhiSVFGNode(uint32_t id, SVFGK k, const ICFGNode* icfg, const llvm::PHINode* phi)
        : SVFGNode(id, k, icfg), phiNode(phi) {}
    
    const llvm::PHINode* getPHINode() const { return phiNode; }
    const llvm::Value* getValue() const override { return phiNode; }
    const llvm::Instruction* getInstruction() const override { return phiNode; }
    
    SVFG_NODE_KIND(Phi)
};

/// @brief Intra-procedural PHI node
class IntraPhiSVFGNode : public PhiSVFGNode
{
public:
    IntraPhiSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::PHINode* phi)
        : PhiSVFGNode(id, SVFGK::IntraPhi, icfg, phi) {}
    
    SVFG_NODE_KIND(IntraPhi)
};

/// @brief Inter-procedural PHI node (parameter/return value flow)
class InterPhiSVFGNode : public PhiSVFGNode
{
private:
    const llvm::Function* func;        // For formal parameter PHI
    const llvm::CallBase* callSite;   // For actual return PHI
    
public:
    /// @brief Construct for formal parameter PHI
    InterPhiSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Function* f)
        : PhiSVFGNode(id, SVFGK::InterPhi, icfg, nullptr), func(f), callSite(nullptr) {}
    
    /// @brief Construct for actual return PHI
    InterPhiSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::CallBase* cs)
        : PhiSVFGNode(id, SVFGK::InterPhi, icfg, nullptr), func(nullptr), callSite(cs) {}
    
    inline bool isFormalParmPHI() const { return func != nullptr; }
    inline bool isActualRetPHI() const { return callSite != nullptr; }
    const llvm::Function* getFunction() const override { return func; }
    
    SVFG_NODE_KIND(InterPhi)
};

/// @brief Base memory SSA node (address-taken variables)
class MSSASVFGNode : public SVFGNode
{
protected:
    uint32_t memReg;       // Memory region
    uint32_t ssaVersion;   // Memory SSA version
    SVFGNodeBS pointsTo;   // Points-to set
    
public:
    MSSASVFGNode(uint32_t id, SVFGK k, const ICFGNode* icfg, uint32_t reg,
                 const SVFGNodeBS& pts, uint32_t ver = 0)
        : SVFGNode(id, k, icfg), memReg(reg), ssaVersion(ver), pointsTo(pts) {}
    
    const SVFGNodeBS* getPointsTo() const override { return &pointsTo; }
    uint32_t getMemReg() const override { return memReg; }
    uint32_t getSSAVersion() const override { return ssaVersion; }
    SVFGNodeBS getDefSVFVars() const override { return pointsTo; }
    
    SVFG_NODE_KIND(MPhi)
};

/// @brief Memory PHI node
class MSSAPhiSVFGNode : public MSSASVFGNode
{
public:
    MSSAPhiSVFGNode(uint32_t id, SVFGK k, const ICFGNode* icfg, uint32_t reg, const SVFGNodeBS& pts)
        : MSSASVFGNode(id, k, icfg, reg, pts) {}
    
    SVFG_NODE_KIND(MPhi)
};

/// @brief Intra-procedural memory PHI
class IntraMSSAPhiSVFGNode : public MSSAPhiSVFGNode
{
private:
    uint32_t version;
    
public:
    IntraMSSAPhiSVFGNode(uint32_t id, const ICFGNode* icfg, uint32_t reg, uint32_t ver, const SVFGNodeBS& pts)
        : MSSAPhiSVFGNode(id, SVFGK::MIntraPhi, icfg, reg, pts), version(ver) {
        this->ssaVersion = ver;
    }
    
    uint32_t getSSAVersion() const override { return version; }
    
    SVFG_NODE_KIND(MIntraPhi)
};

/// @brief Inter-procedural memory PHI
class InterMSSAPhiSVFGNode : public MSSAPhiSVFGNode
{
private:
    const llvm::Function* func;
    const llvm::CallBase* callSite;
    
public:
    InterMSSAPhiSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Function* f, uint32_t reg, const SVFGNodeBS& pts)
        : MSSAPhiSVFGNode(id, SVFGK::MInterPhi, icfg, reg, pts), func(f), callSite(nullptr) {}
    
    InterMSSAPhiSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::CallBase* cs, uint32_t reg, const SVFGNodeBS& pts)
        : MSSAPhiSVFGNode(id, SVFGK::MInterPhi, icfg, reg, pts), func(nullptr), callSite(cs) {}
    
    bool isFormalParmPHI() const { return func != nullptr; }
    bool isActualRetPHI() const { return callSite != nullptr; }
    const llvm::Function* getFunction() const override { return func; }
    
    SVFG_NODE_KIND(MInterPhi)
};

/// @brief Function entry memory in (formal parameter memory)
class FormalInSVFGNode : public MSSASVFGNode
{
private:
    const llvm::Function* func;
    
public:
    FormalInSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Function* f, uint32_t reg, const SVFGNodeBS& pts)
        : MSSASVFGNode(id, SVFGK::FormalIn, icfg, reg, pts), func(f) {}
    
    const llvm::Function* getFunction() const override { return func; }
    
    SVFG_NODE_KIND(FormalIn)
};

/// @brief Function exit memory out (formal return memory)
class FormalOutSVFGNode : public MSSASVFGNode
{
private:
    const llvm::Function* func;
    
public:
    FormalOutSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Function* f, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::FormalOut, icfg, reg, pts, ver), func(f) {}
    
    const llvm::Function* getFunction() const override { return func; }
    
    SVFG_NODE_KIND(FormalOut)
};

/// @brief Callsite actual memory in
class ActualInSVFGNode : public MSSASVFGNode
{
private:
    const llvm::CallBase* callSite;
    
public:
    ActualInSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::CallBase* cs, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::ActualIn, icfg, reg, pts, ver), callSite(cs) {}
    
    inline const llvm::CallBase* getCallSite() const { return callSite; }
    
    SVFG_NODE_KIND(ActualIn)
};

/// @brief Callsite actual memory out
class ActualOutSVFGNode : public MSSASVFGNode
{
private:
    const llvm::CallBase* callSite;
    
public:
    ActualOutSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::CallBase* cs, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::ActualOut, icfg, reg, pts, ver), callSite(cs) {}
    
    inline const llvm::CallBase* getCallSite() const { return callSite; }
    
    SVFG_NODE_KIND(ActualOut)
};

/// @brief Load memory use (MU in SVF terminology)
class LoadMuSVFGNode : public MSSASVFGNode
{
private:
    const llvm::LoadInst* loadInst;
    
public:
    LoadMuSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::LoadInst* load, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::LoadMu, icfg, reg, pts, ver), loadInst(load) {}
    
    const llvm::LoadInst* getLoadInst() const { return loadInst; }
    /// @brief Check if this is from an intrinsic (loadInst may be nullptr)
    bool isFromIntrinsic() const { return loadInst == nullptr; }
    
    SVFG_NODE_KIND(LoadMu)
};

/// @brief Store memory def (CHI in SVF terminology)
class StoreChiSVFGNode : public MSSASVFGNode
{
private:
    const llvm::StoreInst* storeInst;
    
public:
    StoreChiSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::StoreInst* store, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::StoreChi, icfg, reg, pts, ver), storeInst(store) {}
    
    const llvm::StoreInst* getStoreInst() const { return storeInst; }
    /// @brief Check if this is from an intrinsic (storeInst may be nullptr)
    bool isFromIntrinsic() const { return storeInst == nullptr; }
    
    SVFG_NODE_KIND(StoreChi)
};

/// @brief Call memory use
class CallMuSVFGNode : public MSSASVFGNode
{
private:
    const llvm::CallBase* callSite;
    
public:
    CallMuSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::CallBase* cs, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::CallMu, icfg, reg, pts, ver), callSite(cs) {}
    
    inline const llvm::CallBase* getCallSite() const { return callSite; }
    
    SVFG_NODE_KIND(CallMu)
};

/// @brief Call memory def
class CallChiSVFGNode : public MSSASVFGNode
{
private:
    const llvm::CallBase* callSite;
    
public:
    CallChiSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::CallBase* cs, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::CallChi, icfg, reg, pts, ver), callSite(cs) {}
    
    inline const llvm::CallBase* getCallSite() const { return callSite; }
    
    SVFG_NODE_KIND(CallChi)
};

/// @brief Return memory use
class RetMuSVFGNode : public MSSASVFGNode
{
private:
    const llvm::Function* func;
    
public:
    RetMuSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Function* f, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::RetMu, icfg, reg, pts, ver), func(f) {}
    
    const llvm::Function* getFunction() const override { return func; }
    
    SVFG_NODE_KIND(RetMu)
};

/// @brief Entry memory chi (function entry point)
class EntryChiSVFGNode : public MSSASVFGNode
{
private:
    const llvm::Function* func;
    
public:
    EntryChiSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Function* f, uint32_t reg, const SVFGNodeBS& pts, uint32_t ver = 0)
        : MSSASVFGNode(id, SVFGK::EntryChi, icfg, reg, pts, ver), func(f) {}
    
    const llvm::Function* getFunction() const override { return func; }
    
    SVFG_NODE_KIND(EntryChi)
};

/// @brief Formal parameter node
class FormalParmSVFGNode : public SVFGNode
{
private:
    const llvm::Function* func;
    unsigned paramIdx;
    
public:
    FormalParmSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Function* f, unsigned idx)
        : SVFGNode(id, SVFGK::FormalParm, icfg), func(f), paramIdx(idx) {}
    
    const llvm::Function* getFunction() const override { return func; }
    inline unsigned getParamIndex() const { return paramIdx; }
    
    SVFG_NODE_KIND(FormalParm)
};

/// @brief Actual parameter node
class ActualParmSVFGNode : public SVFGNode
{
private:
    const llvm::CallBase* callSite;
    unsigned paramIdx;
    
public:
    ActualParmSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::CallBase* cs, unsigned idx)
        : SVFGNode(id, SVFGK::ActualParm, icfg), callSite(cs), paramIdx(idx) {}
    
    inline const llvm::CallBase* getCallSite() const { return callSite; }
    inline unsigned getParamIndex() const { return paramIdx; }
    
    SVFG_NODE_KIND(ActualParm)
};

/// @brief Formal return node
class FormalRetSVFGNode : public SVFGNode
{
private:
    const llvm::Function* func;
    
public:
    FormalRetSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::Function* f)
        : SVFGNode(id, SVFGK::FormalRet, icfg), func(f) {}
    
    const llvm::Function* getFunction() const override { return func; }
    
    SVFG_NODE_KIND(FormalRet)
};

/// @brief Actual return node
class ActualRetSVFGNode : public SVFGNode
{
private:
    const llvm::CallBase* callSite;
    
public:
    ActualRetSVFGNode(uint32_t id, const ICFGNode* icfg, const llvm::CallBase* cs)
        : SVFGNode(id, SVFGK::ActualRet, icfg), callSite(cs) {}
    
    inline const llvm::CallBase* getCallSite() const { return callSite; }
    
    SVFG_NODE_KIND(ActualRet)
};

/// @brief Null pointer node
class NullPtrSVFGNode : public SVFGNode
{
public:
    NullPtrSVFGNode(uint32_t id, const ICFGNode* icfg)
        : SVFGNode(id, SVFGK::NullPtr, icfg) {}
    
    bool isNullPtrNode() const override { return true; }
    
    SVFG_NODE_KIND(NullPtr)
};

/// @brief Dummy/auxiliary node
class DummySVFGNode : public SVFGNode
{
public:
    DummySVFGNode(uint32_t id, const ICFGNode* icfg)
        : SVFGNode(id, SVFGK::Dummy, icfg) {}
    
    bool isDummyNode() const override { return true; }
    
    SVFG_NODE_KIND(Dummy)
};

} // namespace analysis
} // namespace lotus
