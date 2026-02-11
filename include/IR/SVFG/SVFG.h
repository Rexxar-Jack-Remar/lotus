//===- SVFG.h -- Sparse Value-Flow Graph --------------------------------------//
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
// SVFG: Production-ready Sparse Value-Flow Graph.
//
// This file provides the main SVFG class aligned with SVF's design, supporting:
// - Sparse value-flow representation for whole-program analysis
// - Memory SSA form for precise alias tracking
// - Interprocedural value-flow through calls and returns
// - Integration with pointer analysis (Andersen, etc.)
//
// Key features:
// - Efficient node/edge lookups with multiple indices
// - Full call graph integration
// - Memory SSA versioning
// - Points-to set management
// - Graph algorithms (reachability, slicing)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/SVFG/SVFGBase.h"
#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus {
namespace analysis {

class SVFG;

/// @brief SVFG node ID to node map
using SVFGNodeMap = std::unordered_map<uint32_t, SVFGNode*>;

/// @brief SVFG node set
using SVFGNodeSet = std::set<SVFGNode*>;

/// @brief LLVM instruction to SVFG node ID map for definition sites
using InstToDefMap = std::unordered_map<const llvm::Instruction *, uint32_t>;

/// @brief Value to SVFG node ID map (for top-level pointers)
using ValueToNodeMap = std::unordered_map<const llvm::Value*, uint32_t>;

/// @brief Memory region version to SVFG node ID map
struct MemSSAKey {
    uint32_t memReg = 0;
    uint32_t version = 0;

    bool operator==(const MemSSAKey& other) const {
        return memReg == other.memReg && version == other.version;
    }
};

struct MemSSAKeyHash {
    size_t operator()(const MemSSAKey& key) const noexcept {
        return std::hash<uint32_t>()(key.memReg) ^
               (std::hash<uint32_t>()(key.version) << 1);
    }
};

using MRVerToNodeMap = std::unordered_map<MemSSAKey, SVFGNode*, MemSSAKeyHash>;

/// @brief Callsite instruction to actual-in/out SVFG node set map
using CallSiteToActualMap =
    std::unordered_map<const llvm::CallBase *, SVFGNodeSet>;

/// @brief Callsite instruction to actual parameter/return SVFG node set map
using CallSiteToActualParmMap =
    std::unordered_map<const llvm::CallBase *, SVFGNodeSet>;
using CallSiteToActualRetMap =
    std::unordered_map<const llvm::CallBase *, SVFGNodeSet>;

/// @brief Function to formal-in/out SVFG node set map
using FuncToFormalMap = std::unordered_map<const llvm::Function*, SVFGNodeSet>;

/// @brief Function to formal parameter/return SVFG node set map
using FuncToFormalParmMap =
    std::unordered_map<const llvm::Function *, SVFGNodeSet>;
using FuncToFormalRetMap =
    std::unordered_map<const llvm::Function *, SVFGNodeSet>;

/// @brief SVFG statistics
struct SVFGStat {
    uint32_t numNodes = 0;
    uint32_t numEdges = 0;
    uint32_t numAddrNodes = 0;
    uint32_t numCopyNodes = 0;
    uint32_t numLoadNodes = 0;
    uint32_t numStoreNodes = 0;
    uint32_t numGepNodes = 0;
    uint32_t numPhiNodes = 0;
    uint32_t numMemNodes = 0;
    uint32_t numParamNodes = 0;
    uint32_t numCallEdges = 0;
    uint32_t numRetEdges = 0;
    uint32_t numIntraEdges = 0;
};

/// @brief Main SVFG class
class SVFG
{
public:
    /// @brief Iterator types
    using iterator = SVFGNodeMap::iterator;
    using const_iterator = SVFGNodeMap::const_iterator;

    /// @brief Object metadata for DDA (heap/stack/field-insensitive/etc.).
    struct ObjectInfo {
        bool isHeap = false;
        bool isStack = false;
        bool isGlobal = false;
        bool isFunction = false;
        bool isFieldInsensitive = false;
        bool isArray = false;
        bool isUnknown = false;
        uint32_t baseObjId = 0;
    };

private:
    /// @brief Primary node map: ID -> Node
    SVFGNodeMap nodeMap;
    
    /// @brief Definition mapping: ICFGNode -> SVFGNode ID
    InstToDefMap instToDefMap;
    
    /// @brief Value mapping: LLVM Value -> SVFGNode ID (top-level pointers)
    ValueToNodeMap valueToNodeMap;
    
    /// @brief Memory SSA mapping: Memory region version -> SVFGNode ID
    MRVerToNodeMap mssaVerToNodeMap;
    
    /// @brief Callsite mappings
    CallSiteToActualMap callSiteToActualInMap;   // ActualIn (memory)
    CallSiteToActualMap callSiteToActualOutMap;  // ActualOut (memory)
    CallSiteToActualParmMap callSiteToActualParmMap;
    CallSiteToActualRetMap callSiteToActualRetMap;
    
    /// @brief Function mappings
    FuncToFormalMap funcToFormalInMap;   // FormalIn (memory)
    FuncToFormalMap funcToFormalOutMap;  // FormalOut (memory)
    FuncToFormalParmMap funcToFormalParmMap;
    FuncToFormalRetMap funcToFormalRetMap;
    
    /// @brief Associated ICFG
    const ICFG* icfg;
    
    /// @brief Next available node ID
    uint32_t nextNodeId;
    
    /// @brief Statistics
    SVFGStat stat;

    /// @brief Debug labels for abstract objects used in points-to sets.
    std::unordered_map<uint32_t, std::string> objectDebug;
    std::unordered_map<uint32_t, const llvm::Value *> objIdToValue;
    std::unordered_map<const llvm::Value *, uint32_t> valueToObjId;
    std::unordered_map<uint32_t, ObjectInfo> objIdToInfo;
    std::unordered_map<uint32_t, std::string> nodeFunctionDebug;
    std::unordered_map<uint32_t, std::string> nodeCallSiteDebug;

    /// @brief Indirect-callsite indices used by DDA for on-the-fly call graph refinement.
    std::unordered_map<uint32_t, std::unordered_set<const llvm::CallBase *>>
        funPtrToIndCallSites;
    std::unordered_map<const llvm::CallBase *,
                       std::unordered_set<const llvm::Function *>>
        callSiteToConnectedCallees;
    struct CallSiteCalleeKey {
        const llvm::CallBase *callSite = nullptr;
        const llvm::Function *callee = nullptr;
        bool operator==(const CallSiteCalleeKey &o) const {
            return callSite == o.callSite && callee == o.callee;
        }
    };
    struct CallSiteCalleeKeyHash {
        size_t operator()(const CallSiteCalleeKey &k) const noexcept {
            return std::hash<const void *>()(k.callSite) ^
                   (std::hash<const void *>()(k.callee) << 1);
        }
    };
    std::unordered_map<CallSiteCalleeKey, uint32_t, CallSiteCalleeKeyHash>
        callSiteCalleeToId;
    uint32_t nextCallSiteId = 1;

public:
    /// @brief Constructor
    SVFG() : icfg(nullptr), nextNodeId(0) {}
    
    /// @brief Destructor
    virtual ~SVFG() {
        // Delete edges once (they are owned by the graph, but referenced by nodes).
        std::unordered_set<SVFGEdge*> edges;
        for (auto& pair : nodeMap) {
            if (!pair.second) continue;
            for (SVFGEdge* e : pair.second->getOutEdges()) {
                edges.insert(e);
            }
        }
        for (SVFGEdge* e : edges) {
            delete e;
        }

        for (auto& pair : nodeMap) {
            delete pair.second;
        }
    }

    //===------------------------------------------------------------------===
    // Node access
    //===------------------------------------------------------------------===
    
    inline SVFGNode* getNode(uint32_t id) const {
        auto it = nodeMap.find(id);
        return (it != nodeMap.end()) ? it->second : nullptr;
    }
    
    inline bool hasNode(uint32_t id) const {
        return nodeMap.find(id) != nodeMap.end();
    }
    
    inline uint32_t getNumNodes() const { return nodeMap.size(); }
    inline uint32_t getNextNodeId() { return nextNodeId++; }
    
    iterator begin() { return nodeMap.begin(); }
    iterator end() { return nodeMap.end(); }
    const_iterator begin() const { return nodeMap.begin(); }
    const_iterator end() const { return nodeMap.end(); }

    //===------------------------------------------------------------------===
    // Definition site access
    //===------------------------------------------------------------------===
    
    inline SVFGNode* getDef(const llvm::Instruction* inst) const {
        auto it = instToDefMap.find(inst);
        return (it != instToDefMap.end()) ? getNode(it->second) : nullptr;
    }
    
    inline bool hasDef(const llvm::Instruction* inst) const {
        return instToDefMap.find(inst) != instToDefMap.end();
    }
    
    inline void setDef(const llvm::Instruction* inst, uint32_t nodeId) {
        instToDefMap[inst] = nodeId;
    }
    
    inline SVFGNode* getMSSADef(uint32_t memReg, uint32_t version = 0) const {
        auto it = mssaVerToNodeMap.find(MemSSAKey{memReg, version});
        return (it != mssaVerToNodeMap.end()) ? it->second : nullptr;
    }
    
    inline void setMSSADef(uint32_t memReg, SVFGNode* node, uint32_t version = 0) {
        mssaVerToNodeMap[MemSSAKey{memReg, version}] = node;
    }

    //===------------------------------------------------------------------===
    // Value mapping
    //===------------------------------------------------------------------===
    
    inline SVFGNode* getValueNode(const llvm::Value* val) const {
        auto it = valueToNodeMap.find(val);
        return (it != valueToNodeMap.end()) ? getNode(it->second) : nullptr;
    }
    
    inline void setValueNode(const llvm::Value* val, uint32_t nodeId) {
        valueToNodeMap[val] = nodeId;
    }

    //===------------------------------------------------------------------===
    // Node/edge management
    //===------------------------------------------------------------------===
    
    void addNode(SVFGNode* node);
    SVFGEdge* addEdge(SVFGNode* src, SVFGNode* dst, SVFGEdgeK kind,
                      const llvm::CallBase* callSite = nullptr,
                      const SVFGNodeBS& pointsTo = SVFGNodeBS(),
                      std::string callSiteDebug = std::string());
    void removeEdge(SVFGEdge* edge);
    void removeNode(SVFGNode* node);
    
    //===------------------------------------------------------------------===
    // Call/return site access
    //===------------------------------------------------------------------===
    
    inline const SVFGNodeSet& getActualIns(const llvm::CallBase* cs) const {
        static SVFGNodeSet empty;
        auto it = callSiteToActualInMap.find(cs);
        return (it != callSiteToActualInMap.end()) ? it->second : empty;
    }
    
    inline const SVFGNodeSet& getActualOuts(const llvm::CallBase* cs) const {
        static SVFGNodeSet empty;
        auto it = callSiteToActualOutMap.find(cs);
        return (it != callSiteToActualOutMap.end()) ? it->second : empty;
    }
    
    inline void addActualIn(const llvm::CallBase* cs, SVFGNode* node) {
        callSiteToActualInMap[cs].insert(node);
    }
    
    inline void addActualOut(const llvm::CallBase* cs, SVFGNode* node) {
        callSiteToActualOutMap[cs].insert(node);
    }

    inline const SVFGNodeSet& getActualParms(const llvm::CallBase* cs) const {
        static SVFGNodeSet empty;
        auto it = callSiteToActualParmMap.find(cs);
        return (it != callSiteToActualParmMap.end()) ? it->second : empty;
    }

    inline const SVFGNodeSet& getActualRets(const llvm::CallBase* cs) const {
        static SVFGNodeSet empty;
        auto it = callSiteToActualRetMap.find(cs);
        return (it != callSiteToActualRetMap.end()) ? it->second : empty;
    }

    inline void addActualParm(const llvm::CallBase* cs, SVFGNode* node) {
        callSiteToActualParmMap[cs].insert(node);
    }

    inline void addActualRet(const llvm::CallBase* cs, SVFGNode* node) {
        callSiteToActualRetMap[cs].insert(node);
    }
    
    inline const SVFGNodeSet& getFormalIns(const llvm::Function* f) const {
        static SVFGNodeSet empty;
        auto it = funcToFormalInMap.find(f);
        return (it != funcToFormalInMap.end()) ? it->second : empty;
    }
    
    inline const SVFGNodeSet& getFormalOuts(const llvm::Function* f) const {
        static SVFGNodeSet empty;
        auto it = funcToFormalOutMap.find(f);
        return (it != funcToFormalOutMap.end()) ? it->second : empty;
    }
    
    inline void addFormalIn(const llvm::Function* f, SVFGNode* node) {
        funcToFormalInMap[f].insert(node);
    }
    
    inline void addFormalOut(const llvm::Function* f, SVFGNode* node) {
        funcToFormalOutMap[f].insert(node);
    }

    inline const SVFGNodeSet& getFormalParms(const llvm::Function* f) const {
        static SVFGNodeSet empty;
        auto it = funcToFormalParmMap.find(f);
        return (it != funcToFormalParmMap.end()) ? it->second : empty;
    }

    inline const SVFGNodeSet& getFormalRets(const llvm::Function* f) const {
        static SVFGNodeSet empty;
        auto it = funcToFormalRetMap.find(f);
        return (it != funcToFormalRetMap.end()) ? it->second : empty;
    }

    inline void addFormalParm(const llvm::Function* f, SVFGNode* node) {
        funcToFormalParmMap[f].insert(node);
    }

    inline void addFormalRet(const llvm::Function* f, SVFGNode* node) {
        funcToFormalRetMap[f].insert(node);
    }

    //===------------------------------------------------------------------===
    // ICFG integration
    //===------------------------------------------------------------------===
    
    inline const ICFG* getICFG() const { return icfg; }
    inline void setICFG(const ICFG* i) { icfg = i; }

    //===------------------------------------------------------------------===
    // Statistics
    //===------------------------------------------------------------------===
    
    inline const SVFGStat& getStat() const { return stat; }
    inline SVFGStat& getStat() { return stat; }
    void updateStat(SVFGNode* node);
    void updateStat(SVFGEdge* edge);

    //===------------------------------------------------------------------===
    // Object debug
    //===------------------------------------------------------------------===

    inline void setObjectDebug(uint32_t objId, std::string label) {
        if (objId == 0) return;
        objectDebug[objId] = std::move(label);
    }

    inline const std::unordered_map<uint32_t, std::string>& getObjectDebugMap() const {
        return objectDebug;
    }

    /// @brief Map object ID (from edge pointsTo) to allocation-site Value*.
    /// Populated by the builder when PTA objects have getValue().
    /// Used by DDA for indirect-edge filtering and result conversion.
    void setObjectValue(uint32_t objId, const llvm::Value *v);
    /// @brief Return allocation-site Value* for objId, or nullptr if unknown.
    const llvm::Value *getObjectValue(uint32_t objId) const;
    /// @brief Return object ID for allocation-site Value* v, or 0 if unknown.
    uint32_t getObjectId(const llvm::Value *v) const;

    //===------------------------------------------------------------------===
    // Indirect callsite index (DDA)
    //===------------------------------------------------------------------===

    inline void addIndCallSite(uint32_t funPtrNodeId, const llvm::CallBase *cs) {
        if (!cs) return;
        funPtrToIndCallSites[funPtrNodeId].insert(cs);
    }

    inline const std::unordered_set<const llvm::CallBase *> &
    getIndCallSites(uint32_t funPtrNodeId) const {
        static const std::unordered_set<const llvm::CallBase *> empty;
        auto it = funPtrToIndCallSites.find(funPtrNodeId);
        return (it != funPtrToIndCallSites.end()) ? it->second : empty;
    }

    inline const std::unordered_map<uint32_t,
                                    std::unordered_set<const llvm::CallBase *>> &
    getIndCallSiteMap() const {
        return funPtrToIndCallSites;
    }

    /// @brief Return true if (cs, callee) was newly marked as connected.
    inline bool markConnectedCallee(const llvm::CallBase *cs,
                                    const llvm::Function *callee) {
        if (!cs || !callee) return false;
        auto &s = callSiteToConnectedCallees[cs];
        if (!s.insert(callee).second)
            return false;
        CallSiteCalleeKey key{cs, callee};
        if (callSiteCalleeToId.find(key) == callSiteCalleeToId.end())
            callSiteCalleeToId.emplace(key, nextCallSiteId++);
        return true;
    }

    inline bool hasConnectedCallee(const llvm::CallBase *cs,
                                   const llvm::Function *callee) const {
        auto it = callSiteToConnectedCallees.find(cs);
        if (it == callSiteToConnectedCallees.end())
            return false;
        return it->second.count(callee) != 0;
    }

    inline const std::unordered_set<const llvm::Function *> &
    getConnectedCallees(const llvm::CallBase *cs) const {
        static const std::unordered_set<const llvm::Function *> empty;
        auto it = callSiteToConnectedCallees.find(cs);
        return (it != callSiteToConnectedCallees.end()) ? it->second : empty;
    }

    /// @brief Return callsite ID for (cs, callee); 0 if not connected.
    uint32_t getCallSiteId(const llvm::CallBase *cs,
                           const llvm::Function *callee) const;

    /// @brief Set object metadata for an abstract object ID.
    inline void setObjectInfo(uint32_t objId, const ObjectInfo &info) {
        if (objId == 0) return;
        objIdToInfo[objId] = info;
    }

    /// @brief Update (merge) object metadata for an abstract object ID.
    inline void updateObjectInfo(uint32_t objId, const ObjectInfo &info) {
        if (objId == 0) return;
        auto &dst = objIdToInfo[objId];
        dst.isHeap = dst.isHeap || info.isHeap;
        dst.isStack = dst.isStack || info.isStack;
        dst.isGlobal = dst.isGlobal || info.isGlobal;
        dst.isFunction = dst.isFunction || info.isFunction;
        dst.isFieldInsensitive = dst.isFieldInsensitive || info.isFieldInsensitive;
        dst.isArray = dst.isArray || info.isArray;
        dst.isUnknown = dst.isUnknown || info.isUnknown;
        if (dst.baseObjId == 0)
            dst.baseObjId = info.baseObjId;
    }

    /// @brief Get object metadata (nullptr if unknown).
    inline const ObjectInfo *getObjectInfo(uint32_t objId) const {
        auto it = objIdToInfo.find(objId);
        return (it != objIdToInfo.end()) ? &it->second : nullptr;
    }

    inline bool isHeapObject(uint32_t objId) const {
        if (const ObjectInfo *info = getObjectInfo(objId)) return info->isHeap;
        return false;
    }
    inline bool isStackObject(uint32_t objId) const {
        if (const ObjectInfo *info = getObjectInfo(objId)) return info->isStack;
        return false;
    }
    inline bool isGlobalObject(uint32_t objId) const {
        if (const ObjectInfo *info = getObjectInfo(objId)) return info->isGlobal;
        return false;
    }
    inline bool isFunctionObject(uint32_t objId) const {
        if (const ObjectInfo *info = getObjectInfo(objId)) return info->isFunction;
        return false;
    }
    inline bool isFieldInsensitiveObject(uint32_t objId) const {
        if (const ObjectInfo *info = getObjectInfo(objId)) return info->isFieldInsensitive;
        return false;
    }
    inline bool isArrayObject(uint32_t objId) const {
        if (const ObjectInfo *info = getObjectInfo(objId)) return info->isArray;
        return false;
    }
    inline bool isUnknownObject(uint32_t objId) const {
        if (const ObjectInfo *info = getObjectInfo(objId)) return info->isUnknown;
        return false;
    }

    inline void setNodeFunctionDebug(uint32_t nodeId, std::string label) {
        nodeFunctionDebug[nodeId] = std::move(label);
    }

    inline void setNodeCallSiteDebug(uint32_t nodeId, std::string label) {
        nodeCallSiteDebug[nodeId] = std::move(label);
    }

    inline std::string getNodeFunctionDebug(uint32_t nodeId) const {
        auto it = nodeFunctionDebug.find(nodeId);
        return (it != nodeFunctionDebug.end()) ? it->second : std::string();
    }

    inline std::string getNodeCallSiteDebug(uint32_t nodeId) const {
        auto it = nodeCallSiteDebug.find(nodeId);
        return (it != nodeCallSiteDebug.end()) ? it->second : std::string();
    }

    inline const std::unordered_map<uint32_t, std::string>& getNodeFunctionDebugMap() const {
        return nodeFunctionDebug;
    }

    inline const std::unordered_map<uint32_t, std::string>& getNodeCallSiteDebugMap() const {
        return nodeCallSiteDebug;
    }

    //===------------------------------------------------------------------===
    // Graph algorithms
    //===------------------------------------------------------------------===
    
    /// @brief Get all predecessors (backward reachability)
    SVFGNodeSet getPreds(SVFGNode* node) const;
    
    /// @brief Get all successors (forward reachability)
    SVFGNodeSet getSuccs(SVFGNode* node) const;
    
    /// @brief Check if path exists between two nodes
    bool hasPath(SVFGNode* src, SVFGNode* dst) const;

    /// @brief Get intra-procedural VFG edge from src to dst with given kind.
    /// Used by DDA to find the direct edge from load/store pointer def to load/store.
    /// Returns nullptr if no such edge exists.
    SVFGEdge* getIntraVFGEdge(const SVFGNode* src, const SVFGNode* dst, SVFGEdgeK kind) const;

    /// @brief Get the "LHS" node for direct value flow: the node that defines
    /// the value flowing out of \p node. For stmt/phi/param nodes this is the node itself.
    /// Used by DDA for backtraceAlongDirectVF.
    SVFGNode* getLHSTopLevPtr(SVFGNode* node) const;

    /// @brief Return callsite-ret SVFG node if \p n is one, else nullptr.
    const ActualRetSVFGNode* isCallSiteRetSVFGNode(const SVFGNode* n) const;
    /// @brief Return fun-entry SVFG node if \p n is one, else nullptr.
    const FormalParmSVFGNode* isFunEntrySVFGNode(const SVFGNode* n) const;

    /// @brief Dump to DOT format
    void dump(const std::string& filename) const;

    /// @brief Serialize graph to text
    bool writeToFile(const std::string& filename) const;

    /// @brief Deserialize graph from text
    bool readFromFile(const std::string& filename);
    
    /// @brief Print statistics
    void printStat() const;

    /// @brief Swap graph contents with another SVFG instance.
    void swapWith(SVFG& other);
    
    //===------------------------------------------------------------------===
    // Incremental update support
    //===------------------------------------------------------------------===
    
    /// @brief Mark nodes/edges that depend on points-to information for update
    /// Called when pointer analysis results change
    void markForUpdate(SVFGNode* node);
    
    /// @brief Get all nodes marked for update
    SVFGNodeSet getNodesForUpdate() const;
    
    /// @brief Clear update markers
    void clearUpdateMarkers();
    
private:
    /// @brief Nodes that need updating when PTA changes
    SVFGNodeSet nodesForUpdate;
};

} // namespace analysis
} // namespace lotus
