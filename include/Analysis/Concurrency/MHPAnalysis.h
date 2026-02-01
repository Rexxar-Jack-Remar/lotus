/**
 * @file MHPAnalysis.h
 * @brief Production-ready May-Happen-in-Parallel (MHP) Analysis
 * 
 * This file provides a comprehensive MHP analysis framework for determining
 * which program statements may execute concurrently in a multithreaded program.
 * 
 * Key Features:
 * - Thread-flow graph construction
 * - Fork-join analysis
 * - Lock-based synchronization analysis
 * - Condition variable analysis
 * - Barrier synchronization support
 * - Efficient query interface
 * - Comprehensive debugging support
 * 
 * @author rainoftime
 * @date 2025
 */

#ifndef MHP_ANALYSIS_H
#define MHP_ANALYSIS_H

#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/Cpp11Atomics.h"
#include "Analysis/Concurrency/ThreadAPI.h"
#include "Analysis/Concurrency/ThreadFlowGraph.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mhp {

// ============================================================================
// Forward Declarations
// ============================================================================

class ThreadFlowGraph;
class SyncNode;
class MHPAnalysis;
class ThreadRegionAnalysis;

// ============================================================================
// Type Definitions
// ============================================================================

using InstructionSet = std::unordered_set<const llvm::Instruction *>;
using InstructionVector = std::vector<const llvm::Instruction *>;
using LockID = const llvm::Value *;

// ============================================================================
// Thread Region Analysis
// ============================================================================

/**
 * @brief Divides program into thread regions based on synchronization
 * 
 * A thread region is a maximal sequence of instructions within a single thread
 * that are not separated by any synchronization operations. Regions are the
 * basic units for MHP analysis.
 */
class ThreadRegionAnalysis {
public:
  struct Region {
    size_t region_id;
    ThreadID thread_id;
    SyncNode *start_node;
    SyncNode *end_node;
    InstructionSet instructions;

    // Synchronization constraints
    std::set<size_t> must_precede;    // Regions that must execute before this
    std::set<size_t> must_follow;     // Regions that must execute after this
    std::set<size_t> may_be_parallel; // Regions that may run in parallel
  };

  ThreadRegionAnalysis(const ThreadFlowGraph &tfg)
      : m_tfg(tfg) {}

  void analyze();

  // Query interface
  const Region *getRegion(size_t region_id) const;
  const Region *getRegionContaining(const llvm::Instruction *inst) const;
  const std::vector<std::unique_ptr<Region>> &getAllRegions() const {
    return m_regions;
  }

  void print(llvm::raw_ostream &os) const;

private:
  const ThreadFlowGraph &m_tfg;

  std::vector<std::unique_ptr<Region>> m_regions;
  std::unordered_map<const llvm::Instruction *, const Region *>
      m_inst_to_region;
  
  void identifyRegions();
  void identifyRegionsForThread(ThreadID tid, const llvm::Function *entry);
  void computeOrderingConstraints();
  void computeParallelism();
  
  // CFG-based helpers
  bool isSyncPoint(const llvm::Instruction *inst) const;
  std::vector<const llvm::Instruction *> collectSyncPoints(const llvm::Function *func) const;
};

// ============================================================================
// MHP Analysis
// ============================================================================

/**
 * @brief Main May-Happen-in-Parallel analysis
 * 
 * Computes which pairs of program statements may execute concurrently in a
 * multithreaded program. Takes into account:
 * - Thread creation and termination (fork-join)
 * - Lock-based synchronization
 * - Condition variables
 * - Barriers
 * 
 * Usage:
 *   MHPAnalysis mhp(module);
 *   mhp.analyze();
 *   if (mhp.mayHappenInParallel(inst1, inst2)) {
 *     // inst1 and inst2 may execute concurrently
 *   }
 */
class MHPAnalysis {
public:
  explicit MHPAnalysis(llvm::Module &module);
  ~MHPAnalysis() = default;

  // Main analysis entry point
  void analyze();

  // Alias analysis used internally (also useful for other analyses/checkers).
  lotus::AliasAnalysisWrapper *getAliasAnalysis() const { return m_alias_analysis.get(); }

  // ========================================================================
  // Query Interface
  // ========================================================================

  /**
   * @brief Check if two instructions may execute in parallel
   *
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if i1 and i2 may execute concurrently
   */
  bool mayHappenInParallel(const llvm::Instruction *i1,
                           const llvm::Instruction *i2) const;

  /**
   * @brief Check if a pair is in the precomputed MHP set
   *
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if the pair was precomputed as MHP
   */
  bool isPrecomputedMHP(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const;

  /**
   * @brief Get all instructions that may run in parallel with the given one
   * 
   * @param inst Target instruction
   * @return Set of instructions that may execute concurrently with inst
   */
  InstructionSet getParallelInstructions(const llvm::Instruction *inst) const;

  /**
   * @brief Check if two instructions must execute sequentially
   * 
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if i1 and i2 cannot execute concurrently
   */
  bool mustBeSequential(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const;

  /**
   * @brief Check if instruction i1 must execute before i2
   * 
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if i1 must execute before i2 in all executions
   */
  bool mustPrecede(const llvm::Instruction *i1,
                   const llvm::Instruction *i2) const;

  /**
   * @brief Get the thread ID that an instruction belongs to
   * 
   * @param inst Target instruction
   * @return Thread ID, or kUnknownThread if the instruction is thread-ambiguous
   */
  ThreadID getThreadID(const llvm::Instruction *inst) const;

  /**
   * @brief Get all instructions in a specific thread
   * 
   * @param tid Thread ID
   * @return Set of instructions in the thread
   */
  InstructionSet getInstructionsInThread(ThreadID tid) const;

  /**
   * @brief Get locks held at a specific instruction
   * 
   * @param inst Target instruction
   * @return Set of locks that may be held at inst
   */
  std::set<LockID> getLocksHeldAt(const llvm::Instruction *inst) const;

  // ========================================================================
  // Statistics and Debugging
  // ========================================================================

  struct Statistics {
    size_t num_threads;
    size_t num_forks;
    size_t num_joins;
    size_t num_locks;
    size_t num_unlocks;
    size_t num_regions;
    size_t num_mhp_pairs;
    size_t num_ordered_pairs;

    void print(llvm::raw_ostream &os) const;
  };

  Statistics getStatistics() const;
  void printStatistics(llvm::raw_ostream &os) const;
  void printResults(llvm::raw_ostream &os) const;

  // Component access for advanced users
  const ThreadFlowGraph &getThreadFlowGraph() const { return *m_tfg; }
  const ThreadRegionAnalysis &getThreadRegionAnalysis() const {
    return *m_region_analysis;
  }
  
  // Optional: LockSetAnalysis for more precise race detection
  LockSetAnalysis *getLockSetAnalysis() const { return m_lockset.get(); }
  void enableLockSetAnalysis();

  // Optional: precompute and store the full MHP pair relation.
  // Warning: can be O(N^2) in number of instructions.
  void enableMHPPrecomputation(bool enable = true) { m_precompute_mhp_pairs = enable; }

  // Visualization
  void dumpThreadFlowGraph(const std::string &filename) const;
  void dumpMHPMatrix(llvm::raw_ostream &os) const;

private:
  struct InstPair {
    const llvm::Instruction *a;
    const llvm::Instruction *b;
    bool operator==(const InstPair &o) const { return a == o.a && b == o.b; }
  };
  struct InstPairHash {
    size_t operator()(const InstPair &p) const {
      size_t h1 = std::hash<const void *>{}(p.a);
      size_t h2 = std::hash<const void *>{}(p.b);
      return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
  };

  llvm::Module &m_module;
  ThreadAPI *m_thread_api;

  // Analysis components
  std::unique_ptr<ThreadFlowGraph> m_tfg;
  std::unique_ptr<LockSetAnalysis> m_lockset;  // Optional
  std::unique_ptr<ThreadRegionAnalysis> m_region_analysis;
  
  // Configuration
  bool m_enable_lockset_analysis = false;
  bool m_precompute_mhp_pairs = false;

  // MHP results
  std::set<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      m_mhp_pairs;

  // Atomics-based happens-before cache
  std::vector<const llvm::Instruction *> m_atomic_instructions;
  std::set<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      m_atomic_hb_pairs;

  // Instruction to thread mapping
  std::unordered_map<const llvm::Instruction *, ThreadID> m_inst_to_thread;

  // Alias Analysis
  std::unique_ptr<lotus::AliasAnalysisWrapper> m_alias_analysis;

  // Thread ID allocation
  ThreadID m_next_thread_id = 1; // 0 is reserved for main thread

  // Multi-instance thread tracking
  std::unordered_set<ThreadID> m_multi_instance_threads;

  // Fork-join tracking
  std::unordered_map<ThreadID, const llvm::Instruction *>
      m_thread_fork_sites;                                   // Thread -> fork instruction
  std::unordered_map<ThreadID, ThreadID> m_thread_parents;   // Child -> Parent
  std::unordered_map<ThreadID, std::vector<ThreadID>> m_thread_children; // Parent -> Children
  std::unordered_map<const llvm::Instruction *, ThreadID>
      m_fork_to_thread;                                      // Fork inst -> created thread
  std::unordered_map<const llvm::Instruction *, ThreadID>
      m_join_to_thread;                                      // Join inst -> joined thread
  
  // Value tracking for pthread_t variables
  std::unordered_map<const llvm::Value *, ThreadID>
      m_pthread_value_to_thread;                             // pthread_t value -> thread ID
  std::unordered_map<ThreadID, const llvm::Value *>
      m_thread_to_pthread_value;                             // thread ID -> pthread_t value
  
  // Condition variable tracking (for happens-before)
  // Map condition variable -> list of signal/broadcast instructions
  std::unordered_map<const llvm::Value *, std::vector<const llvm::Instruction *>>
      m_condvar_signals;
  // Map condition variable -> list of wait instructions
  std::unordered_map<const llvm::Value *, std::vector<const llvm::Instruction *>>
      m_condvar_waits;
  
  // Barrier tracking (for happens-before)
  // Map barrier -> list of threads that have reached it
  std::unordered_map<const llvm::Value *, std::vector<const llvm::Instruction *>>
      m_barrier_waits;

  // Per-thread set of functions already processed to avoid reprocessing
  std::unordered_map<ThreadID, std::unordered_set<const llvm::Function *>>
      m_visited_functions_by_thread;

  // Indirect fork handling (conservative)
  bool m_has_unresolved_fork = false;
  std::unordered_set<const llvm::Function *> m_thread_entry_candidates;

  // Dominator tree cache for HB queries within a function
  mutable std::unordered_map<const llvm::Function *, std::unique_ptr<DominatorTree>>
      m_dom_cache;
  mutable std::unordered_map<const llvm::Function *, std::unique_ptr<llvm::PostDominatorTree>>
      m_post_dom_cache;

  // Query caches (keyed by instruction pointer identity).
  mutable std::unordered_map<InstPair, bool, InstPairHash> m_hb_cache;
  mutable std::unordered_map<InstPair, bool, InstPairHash> m_mhp_cache;

  // ========================================================================
  // Analysis Phases
  // ========================================================================

  /**
   * @brief Phase 1: Build thread-flow graph
   * 
   * Constructs a graph representation of all threads, including
   * synchronization operations and inter-thread edges.
   */
  void buildThreadFlowGraph();

  /**
   * @brief Phase 2: Analyze lock sets (OPTIONAL)
   * 
   * Computes the sets of locks held at each program point.
   * Only runs if enableLockSetAnalysis() was called.
   */
  void analyzeLockSets();

  /**
   * @brief Phase 3: Identify thread regions
   * 
   * Divides each thread into regions separated by synchronization.
   */
  void analyzeThreadRegions();

  /**
   * @brief Phase 4: Compute MHP pairs
   * 
   * Determines which pairs of instructions may execute in parallel.
   */
  void computeMHPPairs();

  /**
   * @brief Phase 5: Compute atomic happens-before
   *
   * Finds happens-before relationships created by C++11 atomics.
   */
  void computeAtomicHappensBefore();

  // ========================================================================
  // Helper Methods
  // ========================================================================

  void processFunction(const llvm::Function *func, ThreadID tid);
  void processInstruction(const llvm::Instruction *inst, ThreadID tid,
                          SyncNode *&current_node);

  ThreadID allocateThreadID();
  void mapInstructionToThread(const llvm::Instruction *inst, ThreadID tid);

  // Fork-join analysis
  void handleThreadFork(const llvm::Instruction *fork_inst, SyncNode *node,
                        ThreadID parent_tid);
  void handleThreadJoin(const llvm::Instruction *join_inst, SyncNode *node,
                        ThreadID parent_tid);

  // Value-tracing helper for pthread_t
  const llvm::Value *tracePthreadT(const llvm::Value *val) const;

  // Synchronization analysis
  void handleLockAcquire(const llvm::Instruction *lock_inst, SyncNode *node);
  void handleLockRelease(const llvm::Instruction *unlock_inst, SyncNode *node);
  void handleCondWait(const llvm::Instruction *wait_inst, SyncNode *node);
  void handleCondSignal(const llvm::Instruction *signal_inst, SyncNode *node);
  void handleBarrier(const llvm::Instruction *barrier_inst, SyncNode *node);

  // Ordering computation
  bool hasHappenBeforeRelation(const llvm::Instruction *i1,
                                const llvm::Instruction *i2) const;
  bool isInSameThread(const llvm::Instruction *i1,
                      const llvm::Instruction *i2) const;
  bool isOrderedByLocks(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const;
  bool isInstructionThreadAmbiguous(const llvm::Instruction *inst) const;
  bool isMustIntraThreadEdge(const SyncNode *from, const SyncNode *to) const;


  // Alias analysis helper

  
  
  // Fork-join helper methods
  bool isAncestorThread(ThreadID ancestor, ThreadID descendant) const;
  bool isForkSite(const llvm::Instruction *inst) const;
  bool isJoinSite(const llvm::Instruction *inst) const;
  ThreadID getForkedThreadID(const llvm::Instruction *fork_inst) const;
  ThreadID getJoinedThreadID(const llvm::Instruction *join_inst) const;

  // Dominator helpers (intra-function)
  const DominatorTree &getDomTree(const llvm::Function *func) const;
  const llvm::PostDominatorTree &getPostDomTree(const llvm::Function *func) const;
  bool dominates(const llvm::Instruction *a, const llvm::Instruction *b) const;
  
  // Program order helpers (precise happens-before for same thread)
  bool isReachableWithoutBackEdges(const llvm::Instruction *from,
                                    const llvm::Instruction *to) const;
  bool isBackEdge(const llvm::BasicBlock *from, const llvm::BasicBlock *to,
                  const DominatorTree &DT) const;

  void enableIndirectForkConservatism();

  static constexpr ThreadID kUnknownThread =
      std::numeric_limits<ThreadID>::max();
};

} // namespace mhp

#endif // MHP_ANALYSIS_H
