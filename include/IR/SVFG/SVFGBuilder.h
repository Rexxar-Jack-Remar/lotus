//===- SVFGBuilder.h -- SVFG Builder with AserPTA Integration
//---------------------//
//
//                     Lotus: Static Value-Flow Analysis
//
// Copyright (C) <2025>  <Lotus Development Team>
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
// SVFGBuilder: Production-ready builder using AserPTA for pointer analysis.
//
// This builder integrates with AserPTA (lib/Alias/AserPTA) which provides:
// - Context-insensitive and k-call-site sensitive analysis
// - Field-sensitive and field-insensitive memory models
// - Multiple solver algorithms (Andersen, WavePropagation, DeepPropagation)
// - On-the-fly call graph construction
//
// Default configuration uses:
// - Context-insensitive analysis (NoCtx)
// - Field-sensitive memory model
// - Andersen solver (basic, fast, accurate enough for most cases)
//
// To use different configurations:
// - Context-sensitive: Use KCallSite<1>, KCallSite<2>, etc.
// - Different solver: WavePropagation, DeepPropagation, PartialUpdateSolver
// - Field-insensitive: Use FIMemModel instead of FSMemModel
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"

#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace analysis {

/// @brief SVFGBuilder configuration
struct SVFGBuilderConfig {
  /// @brief Enable Memory SSA construction
  bool buildMSSA = true;

  /// @brief Use pointer analysis integration
  bool usePointerAnalysis = true;

  /// @brief Include global initializers
  bool includeGlobals = true;

  /// @brief Maximum SSA version for memory regions
  uint32_t maxSSAVersion = 16;

  /// @brief Solver type for AserPTA
  enum class SolverType {
    Andersen,        // Basic Andersen (default)
    WavePropagation, // SCC-based with differential propagation
    DeepPropagation, // Enhanced cycle detection
    PartialUpdate    // Hybrid with incremental updates
  } solverType = SolverType::Andersen;

  /// @brief Memory model type
  enum class MemModelType {
    FieldSensitive,  // FSMemModel (default)
    FieldInsensitive // FIMemModel
  } memModelType = MemModelType::FieldSensitive;

  /// @brief Constructor with defaults
  SVFGBuilderConfig() = default;
};

/// @brief SVFGBuilder with AserPTA integration
class SVFGBuilder {
public:
  /// @brief Memory region version info
  struct MemRegVer {
    uint32_t region;
    uint32_t version;
    bool operator==(const MemRegVer &other) const {
      return region == other.region && version == other.version;
    }
  };

  /// @brief Hash for MemRegVer
  struct MemRegVerHash {
    size_t operator()(const MemRegVer &mrv) const noexcept {
      return std::hash<uint32_t>()(mrv.region) ^
             (std::hash<uint32_t>()(mrv.version) << 1);
    }
  };

private:
  /// @brief Configuration
  SVFGBuilderConfig config;

  /// @brief Source ICFG
  const ICFG *icfg;

  /// @brief Built SVFG
  std::unique_ptr<SVFG> svfg;

  /// @brief Type-erased solver wrapper for safe storage and deletion
  struct SolverWrapper {
    enum class SolverKind { Wave, Deep, Basic };
    SolverKind kind;
    void *solver;

    SolverWrapper(SolverKind k, void *s) : kind(k), solver(s) {}
    
    void destroy();
    
    ~SolverWrapper() { destroy(); }
    
    // Non-copyable, movable
    SolverWrapper(const SolverWrapper &) = delete;
    SolverWrapper &operator=(const SolverWrapper &) = delete;
    SolverWrapper(SolverWrapper &&other) noexcept
        : kind(other.kind), solver(other.solver) {
      other.solver = nullptr;
    }
  };

  /// @brief AserPTA solver wrapper (type-safe deletion)
  std::unique_ptr<SolverWrapper> ptaSolverWrapper;
  
  /// @brief Previous points-to sets for change detection
  std::unordered_map<const llvm::Value *, std::vector<const void *>> previousPTSets;

  /// @brief Next node ID
  uint32_t nextNodeId;

  /// @brief Next memory region ID (separate from node IDs)
  uint32_t nextMemRegId;

  /// @brief Value to SVFG node mapping
  std::unordered_map<const llvm::Value *, uint32_t> valueToNode;

  /// @brief PTA object to SVFG node mapping (for points-to set conversion)
  std::unordered_map<const void *, uint32_t> ptaObjectToSVFGNode;

  /// @brief Alloca instruction to memory region mapping
  std::unordered_map<const llvm::AllocaInst *, uint32_t> allocaToMemReg;

  /// @brief Global variable to memory region mapping
  std::unordered_map<const llvm::GlobalVariable *, uint32_t> globalToMemReg;

  /// @brief Load instruction to memory use mapping
  std::unordered_map<const llvm::LoadInst *, uint32_t> loadToMu;

  /// @brief Store instruction to memory def mapping
  std::unordered_map<const llvm::StoreInst *, uint32_t> storeToChi;

  /// @brief Atomic instruction to memory use/def mapping
  std::unordered_map<const llvm::Instruction *, uint32_t> atomicToMu;
  std::unordered_map<const llvm::Instruction *, uint32_t> atomicToChi;

  /// @brief Memory region version mapping
  std::unordered_map<MemRegVer, uint32_t, MemRegVerHash> memRegVerToNode;

  /// @brief Function entry chi nodes
  std::unordered_map<const llvm::Function *, std::vector<uint32_t>>
      funcEntryChi;

  /// @brief Function exit mu nodes
  std::unordered_map<const llvm::Function *, std::vector<uint32_t>> funcExitMu;

  /// @brief Callsite actual-in nodes
  std::unordered_map<const llvm::CallBase *, std::vector<uint32_t>> csActualIn;

  /// @brief Callsite actual-out nodes
  std::unordered_map<const llvm::CallBase *, std::vector<uint32_t>> csActualOut;

  /// @brief Heap allocation to memory region mapping
  std::unordered_map<const llvm::Instruction *, uint32_t> heapAllocToMemReg;

  /// @brief Generic pointer value to memory region mapping fallback
  std::unordered_map<const llvm::Value *, uint32_t> ptrValToMemReg;

  /// @brief Call instruction to CallMu/CallChi nodes
  std::unordered_map<const llvm::CallBase *, uint32_t> callToMu;
  std::unordered_map<const llvm::CallBase *, uint32_t> callToChi;

  /// @brief Memory region version counter (for SSA versioning)
  /// Key: (Function*, memory region) -> version number
  /// This ensures versions are unique per function, avoiding collisions
  std::unordered_map<const llvm::Function *, std::unordered_map<uint32_t, uint32_t>> memRegVersion;

  /// @brief Basic block to memory PHI nodes map
  std::unordered_map<const llvm::BasicBlock *, std::unordered_map<uint32_t, uint32_t>> bbToMemPhi;

  /// @brief Function argument to memory region mapping (for FormalIn/FormalOut)
  std::unordered_map<const llvm::Argument *, uint32_t> argToMemReg;

public:
  /// @brief Constructor
  SVFGBuilder(const SVFGBuilderConfig &cfg = SVFGBuilderConfig())
      : config(cfg), icfg(nullptr), svfg(nullptr), ptaSolverWrapper(nullptr),
        nextNodeId(0), nextMemRegId(1) {}

  /// @brief Destructor
  ~SVFGBuilder();

  // Prevent copying
  SVFGBuilder(const SVFGBuilder &) = delete;
  SVFGBuilder &operator=(const SVFGBuilder &) = delete;

  //===------------------------------------------------------------------===
  // Build API
  //===------------------------------------------------------------------===

  /// @brief Build SVFG from ICFG with AserPTA
  /// @param icfg Input ICFG
  /// @return Built SVFG
  SVFG *build(const ICFG *icfg);

  /// @brief Build SVFG with configuration
  SVFG *build(const ICFG *icfg, const SVFGBuilderConfig &cfg);

  //===------------------------------------------------------------------===
  // Incremental update API
  //===------------------------------------------------------------------===

  /// @brief Update SVFG when pointer analysis results change
  /// This refreshes edges that depend on points-to sets.
  /// Note: For complex updates (StoreChi, PHI nodes, inter-procedural edges),
  /// a full rebuild may be more reliable than incremental update.
  /// @param svfg Existing SVFG to update
  /// @return true if update was successful
  bool updateSVFG(SVFG *svfg);

  /// @brief Update memory SSA edges for nodes marked for update
  void updateMemorySSAEdges(SVFG *svfg);

  //===------------------------------------------------------------------===
  // Builder phases
  //===------------------------------------------------------------------===

private:
  void initialize(const ICFG *cfg);
  void runPointerAnalysis();
  void buildNodes();
  void buildEdges();
  void buildMemorySSA();
  void buildMemoryPHINodes();
  void buildInterproceduralMemoryPHINodes();
  void connectMemorySSAEdges();
  void buildInterproceduralEdges();

  //===------------------------------------------------------------------===
  // Node building
  //===------------------------------------------------------------------===

  void buildTopLevelNodes();
  void buildAddressTakenNodes();
  void buildFormalParmNodes();
  void buildActualParmNodes();
  void buildFormalRetNodes();
  void buildActualRetNodes();

  //===------------------------------------------------------------------===
  // Edge building
  //===------------------------------------------------------------------===

  void buildCopyEdges();
  void buildDirectEdges();
  void buildLoadEdges();
  void buildStoreEdges();
  void buildGepEdges();
  void buildPhiEdges();
  void buildCmpEdges();
  void buildBranchEdges();
  void buildCallEdges();
  void buildReturnEdges();
  void buildMemoryEdges();

  //===------------------------------------------------------------------===
  // Helper methods
  //===------------------------------------------------------------------===

  /// @brief Get or create node ID for a value
  uint32_t getOrCreateNode(const llvm::Value *val);

  /// @brief Get or create memory region for an alloca
  uint32_t getOrCreateMemReg(const llvm::AllocaInst *alloca);

  /// @brief Get or create memory region for a global
  uint32_t getOrCreateMemReg(const llvm::GlobalVariable *global);

  /// @brief Get points-to set for a pointer value using AserPTA
  /// Returns a vector of FSObject pointers
  std::vector<const void *> getPointsToSet(const llvm::Value *ptr);

  /// @brief Convert PTA objects to SVFG node IDs (for points-to sets)
  /// Maps PTA objects to SVFG nodes representing those objects
  SVFGNodeBS convertPTAObjectsToSVFGNodes(const std::vector<const void *> &ptaObjects);

  /// @brief Get or create memory region for a heap allocation
  uint32_t getOrCreateMemReg(const llvm::Instruction *heapAlloc);

  /// @brief Get or create memory region for any pointer value
  uint32_t getOrCreateMemReg(const llvm::Value *ptrVal);

  /// @brief Get indirect call targets using pointer analysis
  std::vector<const llvm::Function *> getIndirectCallTargets(const llvm::CallBase *call);

  /// @brief Check if instruction is a heap allocation (malloc/calloc/realloc)
  bool isHeapAllocation(const llvm::Instruction *inst) const;

  /// @brief Check whether pointer value should participate in Memory SSA.
  bool isAddressTakenPointer(const llvm::Value *ptr) const;

  /// @brief Return true when two memory nodes may alias via points-to overlap.
  bool mayAliasMemoryNodes(const MSSASVFGNode *lhs, const MSSASVFGNode *rhs) const;

  /// @brief Create memory SSA version node
  uint32_t createMemRegVerNode(uint32_t memReg, uint32_t version,
                               const ICFGNode *icfgNode);

  /// @brief Create PHI node for a memory region at a merge point
  uint32_t createMemoryPHI(uint32_t memReg, const llvm::BasicBlock *bb);

  /// @brief Check if function might modify memory (basic function summary)
  bool mayModifyMemory(const llvm::Function *F);
  bool mayModifyMemory(const llvm::Function *F,
                       std::unordered_set<const llvm::Function *> &visited);

  /// @brief Get next node ID
  uint32_t nextNode() { return nextNodeId++; }
};

} // namespace analysis
} // namespace lotus
