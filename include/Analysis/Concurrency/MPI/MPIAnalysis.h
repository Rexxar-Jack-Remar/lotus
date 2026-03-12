/**
 * @file MPIAnalysis.h
 * @brief MPI (Message Passing Interface) Program Analysis
 *
 * This file provides analysis capabilities for MPI parallel programs.
 * MPI uses SPMD (Single Program Multiple Data) model where each process
 * has its own address space and communicates via explicit message passing.
 *
 * Key Features:
 * - Process-level concurrency modeling (not thread-based)
 * - Point-to-point communication tracking (blocking and non-blocking)
 * - Collective operation matching
 * - RMA (Remote Memory Access) analysis
 * - Deadlock detection (circular send/recv dependencies)
 * - Collective mismatch detection
 * - Orphaned non-blocking operation detection
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_ANALYSIS_H
#define MPI_ANALYSIS_H

#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mpi {

// ============================================================================
// Forward Declarations
// ============================================================================

class MPIProcessModel;
class MPICollectiveAnalysis;
class MPIRMAAnalysis;

// ============================================================================
// Type Definitions
// ============================================================================

using ProcessID = int; // MPI rank (can be symbolic for static analysis)
using CommunicatorID = const llvm::Value*; // MPI_Comm handle
using RequestID = const llvm::Value*; // MPI_Request handle
using WindowID = const llvm::Value*; // MPI_Win handle

// ============================================================================
// MPI Operation Types
// ============================================================================

/**
 * @brief Represents different categories of MPI operations
 */
enum class MPIOpKind {
  INIT,              ///< MPI_Init/MPI_Init_thread
  FINALIZE,          ///< MPI_Finalize
  SEND_BLOCKING,     ///< Blocking send (MPI_Send, etc.)
  RECV_BLOCKING,     ///< Blocking recv (MPI_Recv)
  SEND_NONBLOCKING,  ///< Non-blocking send (MPI_Isend, etc.)
  RECV_NONBLOCKING,  ///< Non-blocking recv (MPI_Irecv)
  WAIT,              ///< Wait operations (MPI_Wait, MPI_Waitall, etc.)
  TEST,              ///< Test operations (MPI_Test, etc.)
  BARRIER_BLOCKING,  ///< MPI_Barrier
  BARRIER_NONBLOCKING, ///< MPI_Ibarrier
  COLLECTIVE_BLOCKING, ///< Blocking collective operations
  COLLECTIVE_NONBLOCKING, ///< Non-blocking collective operations
  RMA_DATA,          ///< RMA data operations (Put/Get/Accumulate)
  RMA_SYNC,          ///< RMA synchronization
  COMM_MANAGEMENT,   ///< Communicator operations
  REQUEST_MANAGEMENT, ///< MPI_Request_free / MPI_Cancel
  UNKNOWN
};

// ============================================================================
// MPI Operation Info
// ============================================================================

/**
 * @brief Stores information about an MPI operation
 */
struct MPIOperation {
  const llvm::Instruction* inst;
  MPIOpKind kind;
  ThreadAPI::TD_TYPE td_type;
  
  // Context information
  const llvm::Function* function;
  CommunicatorID communicator = nullptr;
  
  // For point-to-point operations
  int source_rank = -1; // -1 means any source (MPI_ANY_SOURCE)
  int dest_rank = -1;   // -1 means unknown
  int tag = -1;         // -1 means any tag (MPI_ANY_TAG)
  
  // For non-blocking operations
  RequestID request = nullptr;
  const llvm::Instruction* completion_inst = nullptr; // Matching wait/test
  
  // For RMA operations
  WindowID window = nullptr;
  int target_rank = -1;
  
  MPIOperation() = default;
  MPIOperation(const llvm::Instruction* i, MPIOpKind k, ThreadAPI::TD_TYPE t)
    : inst(i), kind(k), td_type(t), function(i->getFunction()) {}
};

// ============================================================================
// MPI Process Model
// ============================================================================

/**
 * @brief Models MPI process behavior and communication patterns
 * 
 * Unlike thread-based models, MPI uses SPMD where each process executes
 * the same program but may take different paths based on its rank.
 */
class MPIProcessModel {
public:
  /**
   * @brief Information about a single MPI process
   */
  struct ProcessInfo {
    ProcessID rank;
    CommunicatorID default_comm = nullptr;
    std::vector<MPIOperation> operations;
    std::set<const llvm::Instruction*> collective_ops;
    std::set<RequestID> pending_requests;
  };
  
  /**
   * @brief Information about a non-blocking operation
   */
  struct NonBlockingOp {
    const llvm::Instruction* issue_inst;  // MPI_Isend/Irecv
    RequestID request;
    bool is_completed = false;
    const llvm::Instruction* wait_inst = nullptr; // MPI_Wait/Test
    
    // Communication details
    int peer_rank = -1;
    int tag = -1;
    CommunicatorID comm = nullptr;
  };

  MPIProcessModel(llvm::Module& M, ThreadAPI* api)
    : module_(M), thread_api_(api) {}

  /**
   * @brief Analyze the module to extract MPI operations
   */
  void analyzeModule();

  /**
   * @brief Get all MPI operations in the program
   */
  const std::vector<MPIOperation>& getAllOperations() const { 
    return all_operations_; 
  }

  /**
   * @brief Get operations of a specific kind
   */
  std::vector<MPIOperation> getOperationsByKind(MPIOpKind kind) const;

  /**
   * @brief Check if two operations can communicate
   * 
   * For sends and receives, checks if they can match based on
   * rank, tag, and communicator.
   */
  bool canCommunicate(const MPIOperation& op1, const MPIOperation& op2) const;

  /**
   * @brief Find all non-blocking operations without matching wait
   */
  std::vector<NonBlockingOp> findOrphanedNonBlockingOps() const;

  /**
   * @brief Find potential deadlocks (circular dependencies)
   * 
   * Returns pairs of operations that form deadlock cycles.
   */
  std::vector<std::pair<const llvm::Instruction*, const llvm::Instruction*>>
    findPotentialDeadlocks() const;

private:
  llvm::Module& module_;
  ThreadAPI* thread_api_;
  
  std::vector<MPIOperation> all_operations_;
  std::map<RequestID, NonBlockingOp> non_blocking_ops_;
  
  // Helper methods
  MPIOpKind classifyOperation(const llvm::Instruction* inst,
                              ThreadAPI::TD_TYPE type) const;
  void extractOperationDetails(MPIOperation& op);
  void matchNonBlockingOps();
};

// ============================================================================
// MPI Collective Analysis
// ============================================================================

/**
 * @brief Analyzes collective operations for correctness
 * 
 * All processes in a communicator must call the same collective
 * operation with compatible parameters.
 */
class MPICollectiveAnalysis {
public:
  struct CollectiveCall {
    const llvm::Instruction* inst;
    ThreadAPI::TD_TYPE type;
    CommunicatorID comm;
    const llvm::Function* function;
    
    // For rank-specific collectives
    int root_rank = -1; // For operations like MPI_Bcast, MPI_Gather
  };

  MPICollectiveAnalysis(const MPIProcessModel& model)
    : process_model_(model) {}

  /**
   * @brief Analyze collective operations for mismatches
   */
  void analyzeCollectives();

  /**
   * @brief Check if collectives are matched across all processes
   * 
   * Returns pairs of mismatched collective calls.
   */
  std::vector<std::pair<CollectiveCall, CollectiveCall>> 
    findMismatchedCollectives() const;

  /**
   * @brief Find collectives that may not be reached by all processes
   * 
   * E.g., collective inside a rank-dependent if statement.
   */
  std::vector<const llvm::Instruction*> findConditionalCollectives() const;

private:
  const MPIProcessModel& process_model_;
  std::vector<CollectiveCall> collective_calls_;
  
  bool areCollectivesCompatible(const CollectiveCall& c1, 
                                const CollectiveCall& c2) const;
};

// ============================================================================
// MPI RMA (Remote Memory Access) Analysis
// ============================================================================

/**
 * @brief Analyzes RMA operations for data races and synchronization errors
 * 
 * RMA operations access remote memory directly without involving the
 * target process. Proper synchronization (fence/lock/PSCW) is critical.
 */
class MPIRMAAnalysis {
public:
  enum class SyncModel {
    FENCE,        ///< Active target (MPI_Win_fence)
    LOCK_UNLOCK,  ///< Passive target (MPI_Win_lock/unlock)
    PSCW,         ///< General purpose (Post-Start-Complete-Wait)
    NONE          ///< No synchronization (error!)
  };

  struct RMAWindow {
    WindowID window;
    const llvm::Instruction* create_inst;
    const llvm::Instruction* free_inst = nullptr;
    
    std::set<const llvm::Instruction*> put_ops;
    std::set<const llvm::Instruction*> get_ops;
    std::set<const llvm::Instruction*> accumulate_ops;
    
    // Synchronization tracking
    std::set<const llvm::Instruction*> fence_ops;
    std::set<const llvm::Instruction*> lock_ops;
    std::set<const llvm::Instruction*> unlock_ops;
    std::set<const llvm::Instruction*> flush_ops;
  };

  struct RMAOperation {
    const llvm::Instruction* inst;
    WindowID window;
    int target_rank = -1;
    SyncModel sync_model = SyncModel::NONE;
    
    // Synchronization epoch tracking
    const llvm::Instruction* sync_start = nullptr; // Fence/Lock/Start
    const llvm::Instruction* sync_end = nullptr;   // Fence/Unlock/Complete
  };

  MPIRMAAnalysis(const MPIProcessModel& model, ThreadAPI* api)
    : process_model_(model), thread_api_(api) {}

  /**
   * @brief Analyze RMA operations
   */
  void analyzeRMA();

  /**
   * @brief Find RMA operations without proper synchronization
   */
  std::vector<RMAOperation> findUnsynchronizedRMAOps() const;

  /**
   * @brief Find potential RMA data races
   * 
   * Two conflicting RMA operations (at least one write) to the same
   * window without proper synchronization.
   */
  std::vector<std::pair<RMAOperation, RMAOperation>> 
    findRMARaces() const;

  /**
   * @brief Find windows that are not properly freed
   */
  std::vector<WindowID> findLeakedWindows() const;

private:
  const MPIProcessModel& process_model_;
  ThreadAPI* thread_api_;
  
  std::map<WindowID, RMAWindow> windows_;
  std::vector<RMAOperation> rma_operations_;
  
  SyncModel determineSyncModel(const RMAOperation& op) const;
  bool areRMAOpsConflicting(const RMAOperation& op1, 
                           const RMAOperation& op2) const;
};

// ============================================================================
// Main MPI Analysis Class
// ============================================================================

/**
 * @brief Top-level MPI analysis coordinator
 */
class MPIAnalysis {
public:
  MPIAnalysis(llvm::Module& M) 
    : module_(M), 
      thread_api_(ThreadAPI::getThreadAPI()),
      process_model_(M, thread_api_),
      collective_analysis_(process_model_),
      rma_analysis_(process_model_, thread_api_) {}

  /**
   * @brief Run all MPI analyses
   */
  void runAnalysis();

  /**
   * @brief Print analysis results
   */
  void printResults(llvm::raw_ostream& OS) const;

  /**
   * @brief Get detected issues
   */
  struct AnalysisResults {
    std::vector<MPIProcessModel::NonBlockingOp> orphaned_requests;
    std::vector<std::pair<const llvm::Instruction*, const llvm::Instruction*>> 
      potential_deadlocks;
    std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall, 
                         MPICollectiveAnalysis::CollectiveCall>> 
      mismatched_collectives;
    std::vector<const llvm::Instruction*> conditional_collectives;
    std::vector<MPIRMAAnalysis::RMAOperation> unsynchronized_rma;
    std::vector<std::pair<MPIRMAAnalysis::RMAOperation, 
                         MPIRMAAnalysis::RMAOperation>> 
      rma_races;
    std::vector<WindowID> leaked_windows;
  };

  const AnalysisResults& getResults() const { return results_; }

  // Individual analysis accessors
  const MPIProcessModel& getProcessModel() const { return process_model_; }
  const MPICollectiveAnalysis& getCollectiveAnalysis() const { 
    return collective_analysis_; 
  }
  const MPIRMAAnalysis& getRMAAnalysis() const { return rma_analysis_; }

private:
  llvm::Module& module_;
  ThreadAPI* thread_api_;
  
  MPIProcessModel process_model_;
  MPICollectiveAnalysis collective_analysis_;
  MPIRMAAnalysis rma_analysis_;
  
  AnalysisResults results_;
};

} // namespace mpi

#endif // MPI_ANALYSIS_H
