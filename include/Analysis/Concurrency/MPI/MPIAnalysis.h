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

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

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
using CommunicatorID = const llvm::Value *; // MPI_Comm handle
using RequestID = const llvm::Value *;      // MPI_Request handle
using WindowID = const llvm::Value *;       // MPI_Win handle

enum class MPICommunicationMatch { NoMatch, MustMatch, MayMatch, Unknown };

enum class RequestCompletionState {
  Pending,
  MayComplete,
  MustComplete,
  Terminal
};

enum class ProtocolReachability { AllRanks, SomeRanks, Unknown };

enum class RMAEpochKind { None, Access, Exposure };

// ============================================================================
// MPI Operation Types
// ============================================================================

/**
 * @brief Represents different categories of MPI operations
 */
enum class MPIOpKind {
  INIT,                   ///< MPI_Init/MPI_Init_thread
  FINALIZE,               ///< MPI_Finalize
  SEND_BLOCKING,          ///< Blocking send (MPI_Send, etc.)
  RECV_BLOCKING,          ///< Blocking recv (MPI_Recv)
  PROBE_BLOCKING,         ///< Blocking probe (MPI_Probe)
  SEND_NONBLOCKING,       ///< Non-blocking send (MPI_Isend, etc.)
  RECV_NONBLOCKING,       ///< Non-blocking recv (MPI_Irecv)
  PROBE_NONBLOCKING,      ///< Non-blocking probe (MPI_Iprobe)
  WAIT,                   ///< Wait operations (MPI_Wait, MPI_Waitall, etc.)
  TEST,                   ///< Test operations (MPI_Test, etc.)
  BARRIER_BLOCKING,       ///< MPI_Barrier
  BARRIER_NONBLOCKING,    ///< MPI_Ibarrier
  COLLECTIVE_BLOCKING,    ///< Blocking collective operations
  COLLECTIVE_NONBLOCKING, ///< Non-blocking collective operations
  RMA_WINDOW,             ///< RMA window lifecycle operations
  RMA_DATA,               ///< RMA data operations (Put/Get/Accumulate)
  RMA_SYNC,               ///< RMA synchronization
  COMM_MANAGEMENT,        ///< Communicator operations
  INTERCOMM_CREATION,     ///< MPI_Intercomm_create, MPI_Intercomm_merge
  REQUEST_MANAGEMENT,     ///< MPI_Request_free / MPI_Cancel
  DATATYPE_CREATE,        ///< MPI_Type_create_* operations
  UNKNOWN
};

// ============================================================================
// MPI Operation Info
// ============================================================================

/**
 * @brief Stores information about an MPI operation
 */
struct MPIOperation {
  const llvm::Instruction *inst;
  MPIOpKind kind;
  ThreadAPI::TD_TYPE td_type;

  // Context information
  const llvm::Function *function;
  CommunicatorID communicator = nullptr;
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t protocol_sequence_id = 0;
  ProtocolReachability protocol_reachability = ProtocolReachability::Unknown;
  MPI::RankExpr process_rank;
  std::string rank_path_summary;
  bool is_intercommunicator = false; ///< True if using intercommunicator

  // For point-to-point operations
  int source_rank = -1; // -1/-2 are treated as wildcard source in matching
  int dest_rank = -1;   // -1 means unknown
  int tag = -1;         // -1/-2 are treated as wildcard tag in matching
  int source_rank_min = -1;
  int source_rank_max = -1;
  int dest_rank_min = -1;
  int dest_rank_max = -1;

  // For non-blocking operations
  RequestID request = nullptr;
  const llvm::Instruction *completion_inst = nullptr; // Matching wait/test
  RequestCompletionState request_state = RequestCompletionState::Pending;

  // For RMA operations
  WindowID window = nullptr;
  int target_rank = -1;
  int target_rank_min = -1;
  int target_rank_max = -1;
  int64_t target_disp = -1;
  int64_t byte_length = -1;

  // For datatype tracking
  const llvm::Value *datatype = nullptr; ///< MPI datatype handle
  int64_t datatype_size = -1; ///< Computed size in bytes (-1 if unknown)
  bool is_derived_datatype =
      false; ///< True if derived (contiguous, vector, etc.)

  RMAEpochKind rma_epoch_kind = RMAEpochKind::None;
  concurrency::ProofStrength synchronization_proof =
      concurrency::ProofStrength::Unknown;

  MPIOperation() = default;
  MPIOperation(const llvm::Instruction *i, MPIOpKind k, ThreadAPI::TD_TYPE t)
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
    std::set<const llvm::Instruction *> collective_ops;
    std::set<RequestID> pending_requests;
  };

  /**
   * @brief Information about a non-blocking operation
   */
  struct NonBlockingOp {
    const llvm::Instruction *issue_inst; // MPI_Isend/Irecv
    RequestID request;
    RequestCompletionState completion_state = RequestCompletionState::Pending;
    const llvm::Instruction *wait_inst = nullptr; // MPI_Wait/Test

    // Communication details
    int peer_rank = -1;
    int tag = -1;
    CommunicatorID comm = nullptr;
  };

  MPIProcessModel(llvm::Module &M, ThreadAPI *api)
      : module_(M), thread_api_(api) {}

  /**
   * @brief Analyze the module to extract MPI operations
   */
  void analyzeModule();

  /**
   * @brief Get all MPI operations in the program
   */
  const std::vector<MPIOperation> &getAllOperations() const {
    return all_operations_;
  }

  const std::unordered_map<MPIOpKind, size_t> &getOperationKindCounts() const {
    return operation_kind_counts_;
  }

  const llvm::Module &getModule() const { return module_; }

  const std::unordered_map<std::string, size_t> &
  getDeferredLoweringStats() const {
    return deferred_lowering_stats_;
  }

  size_t getCommunicatorClassID(CommunicatorID communicator) const;

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
  bool canCommunicate(const MPIOperation &op1, const MPIOperation &op2) const;
  MPICommunicationMatch
  classifyCommunicationMatch(const MPIOperation &op1,
                             const MPIOperation &op2) const;

  /**
   * @brief Find all non-blocking operations without matching wait
   */
  std::vector<NonBlockingOp> findOrphanedNonBlockingOps() const;

  /**
   * @brief Find potential deadlocks (circular dependencies)
   *
   * Returns pairs of operations that form deadlock cycles.
   */
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findPotentialDeadlocks() const;

private:
  llvm::Module &module_;
  ThreadAPI *thread_api_;

  std::vector<MPIOperation> all_operations_;
  std::map<RequestID, NonBlockingOp> non_blocking_ops_;
  std::map<RequestID, NonBlockingOp> persistent_request_templates_;
  std::unordered_map<MPIOpKind, size_t> operation_kind_counts_;
  std::unordered_map<const llvm::Value *, CommunicatorID>
      canonical_communicators_;
  std::unordered_map<const llvm::Value *, size_t> communicator_class_ids_;
  mutable size_t next_communicator_class_id_ = 1;
  std::unique_ptr<MPI::MPIRankAnalysis> rank_analysis_;

  // Helper methods
  MPIOpKind classifyOperation(const llvm::Instruction *inst,
                              ThreadAPI::TD_TYPE type) const;
  void extractOperationDetails(MPIOperation &op);
  bool tryGetConstantInt(const llvm::Value *value, int &out) const;
  void extractPointToPointDetails(MPIOperation &op, const llvm::CallBase *cb);
  void extractSendrecvDetails(MPIOperation &op, const llvm::CallBase *cb) const;
  void extractProbeDetails(MPIOperation &op, const llvm::CallBase *cb) const;
  void extractCollectiveDetails(MPIOperation &op,
                                const llvm::CallBase *cb) const;
  void extractRequestDetails(MPIOperation &op, const llvm::CallBase *cb) const;
  void extractRMAWindowDetails(MPIOperation &op, const llvm::CallBase *cb,
                               llvm::StringRef callee_name) const;
  void extractRMADataDetails(MPIOperation &op, const llvm::CallBase *cb,
                             llvm::StringRef callee_name) const;
  void extractRMASyncDetails(MPIOperation &op, const llvm::CallBase *cb,
                             llvm::StringRef callee_name) const;
  std::vector<RequestID>
  collectRequestOperands(const llvm::Value *request_arg,
                         const llvm::Instruction *context) const;
  std::vector<int>
  collectCompletedRequestIndices(const llvm::Value *indices_arg, size_t bound,
                                 const llvm::Instruction *context) const;
  bool tryReadScalarInt(const llvm::Value *scalar_arg, int &out,
                        const llvm::Instruction *context) const;
  CommunicatorID
  canonicalizeCommunicator(const llvm::Value *communicator) const;
  void registerCommunicatorAlias(const llvm::Value *alias,
                                 const llvm::Value *root);
  size_t assignCommunicatorClass(CommunicatorID canonical);
  void annotateRankConstraints(MPIOperation &op) const;
  int64_t getDatatypeExtent(const llvm::Value *datatype_arg,
                            const llvm::Instruction *context) const;
  void matchNonBlockingOps();
  std::unordered_map<std::string, size_t> deferred_lowering_stats_;
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
    const llvm::Instruction *inst;
    ThreadAPI::TD_TYPE type;
    CommunicatorID comm;
    size_t communicator_class_id = 0;
    const llvm::Function *function;
    size_t sequence_index = 0;

    // For rank-specific collectives
    int root_rank = -1; // For operations like MPI_Bcast, MPI_Gather
    int count = -1;
    int recv_count = -1;
    int datatype = -1;
    int recv_datatype = -1;
    int reduction_op = -1;
    bool in_place = false;
  };

  MPICollectiveAnalysis(const MPIProcessModel &model) : process_model_(model) {}

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
  std::vector<const llvm::Instruction *> findConditionalCollectives() const;
  const std::unordered_map<std::string, size_t> &
  getProtocolDiagnostics() const {
    return protocol_diagnostics_;
  }

private:
  const MPIProcessModel &process_model_;
  std::vector<CollectiveCall> collective_calls_;
  mutable std::unordered_map<std::string, size_t> protocol_diagnostics_;

  bool areCollectivesCompatible(const CollectiveCall &c1,
                                const CollectiveCall &c2) const;
  static int getRootArgIndex(ThreadAPI::TD_TYPE type);
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
    FENCE,       ///< Active target (MPI_Win_fence)
    LOCK_UNLOCK, ///< Passive target (MPI_Win_lock/unlock)
    PSCW,        ///< General purpose (Post-Start-Complete-Wait)
    NONE         ///< No synchronization (error!)
  };

  struct RMAWindow {
    WindowID window;
    const llvm::Instruction *create_inst;
    const llvm::Instruction *free_inst = nullptr;

    std::set<const llvm::Instruction *> put_ops;
    std::set<const llvm::Instruction *> get_ops;
    std::set<const llvm::Instruction *> accumulate_ops;

    // Synchronization tracking
    std::set<const llvm::Instruction *> fence_ops;
    std::set<const llvm::Instruction *> lock_ops;
    std::set<const llvm::Instruction *> unlock_ops;
    std::set<const llvm::Instruction *> flush_ops;
  };

  struct RMAOperation {
    const llvm::Instruction *inst;
    const llvm::Function *function = nullptr;
    WindowID window;
    int target_rank = -1;
    int target_rank_min = -1;
    int target_rank_max = -1;
    int64_t target_disp = -1;
    int64_t byte_length = -1;
    RMAEpochKind rma_epoch_kind = RMAEpochKind::None;
    concurrency::ProofStrength synchronization_proof =
        concurrency::ProofStrength::Unknown;
    SyncModel sync_model = SyncModel::NONE;
    size_t epoch_id = 0;

    // Synchronization epoch tracking
    const llvm::Instruction *sync_start = nullptr; // Fence/Lock/Start
    const llvm::Instruction *sync_end = nullptr;   // Fence/Unlock/Complete
  };

  MPIRMAAnalysis(const MPIProcessModel &model, ThreadAPI *api)
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
  std::vector<std::pair<RMAOperation, RMAOperation>> findRMARaces() const;

  /**
   * @brief Find windows that are not properly freed
   */
  std::vector<WindowID> findLeakedWindows() const;
  size_t getTrackedWindowCount() const { return windows_.size(); }

private:
  const MPIProcessModel &process_model_;
  ThreadAPI *thread_api_;

  std::map<WindowID, RMAWindow> windows_;
  std::vector<RMAOperation> rma_operations_;

  SyncModel determineSyncModel(const RMAOperation &op) const;
  bool areRMAOpsConflicting(const RMAOperation &op1,
                            const RMAOperation &op2) const;
};

// ============================================================================
// Main MPI Analysis Class
// ============================================================================

/**
 * @brief Top-level MPI analysis coordinator
 */
class MPIAnalysis {
public:
  MPIAnalysis(llvm::Module &M)
      : module_(M), thread_api_(ThreadAPI::getThreadAPI()),
        process_model_(M, thread_api_), collective_analysis_(process_model_),
        rma_analysis_(process_model_, thread_api_) {}

  /**
   * @brief Run all MPI analyses
   */
  void runAnalysis();

  /**
   * @brief Print analysis results
   */
  void printResults(llvm::raw_ostream &OS) const;

  /**
   * @brief Get detected issues
   */
  struct AnalysisResults {
    std::vector<MPIProcessModel::NonBlockingOp> orphaned_requests;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        potential_deadlocks;
    std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall,
                          MPICollectiveAnalysis::CollectiveCall>>
        mismatched_collectives;
    std::vector<const llvm::Instruction *> conditional_collectives;
    std::vector<MPIRMAAnalysis::RMAOperation> unsynchronized_rma;
    std::vector<
        std::pair<MPIRMAAnalysis::RMAOperation, MPIRMAAnalysis::RMAOperation>>
        rma_races;
    std::vector<WindowID> leaked_windows;
  };

  const AnalysisResults &getResults() const { return results_; }

  size_t getProtocolDiagnosticCount(llvm::StringRef key) const;
  size_t getOperationCount(MPIOpKind kind) const;
  size_t getTrackedWindowCount() const;

  // Individual analysis accessors
  const MPIProcessModel &getProcessModel() const { return process_model_; }
  const MPICollectiveAnalysis &getCollectiveAnalysis() const {
    return collective_analysis_;
  }
  const MPIRMAAnalysis &getRMAAnalysis() const { return rma_analysis_; }

private:
  llvm::Module &module_;
  ThreadAPI *thread_api_;

  MPIProcessModel process_model_;
  MPICollectiveAnalysis collective_analysis_;
  MPIRMAAnalysis rma_analysis_;

  AnalysisResults results_;
};

} // namespace mpi

#endif // MPI_ANALYSIS_H
