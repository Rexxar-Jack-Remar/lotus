/**
 * @file MPIProcessModel.h
 * @brief MPI Process Model and Behavior Analysis
 *
 * This file provides the MPI process model that tracks MPI operations,
 * non-blocking operations, and communication patterns.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_PROCESS_MODEL_H
#define MPI_PROCESS_MODEL_H

#include "Analysis/Concurrency/MPI/MPIOperation.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Module.h>

namespace mpi {

class MPIRMAAnalysis;

// ============================================================================
// MPI Process Model
// ============================================================================

class MPIProcessModel {
public:
  struct ProcessInfo {
    ProcessID rank;
    CommunicatorID default_comm = nullptr;
    std::vector<MPIOperation> operations;
    std::set<const llvm::Instruction *> collective_ops;
    std::set<RequestID> pending_requests;
  };

  struct NonBlockingOp {
    const llvm::Instruction *issue_inst;
    RequestID request;
    RequestCompletionState completion_state = RequestCompletionState::Pending;
    const llvm::Instruction *wait_inst = nullptr;

    int peer_rank = -1;
    int tag = -1;
    CommunicatorID comm = nullptr;
  };

  MPIProcessModel(llvm::Module &M, ThreadAPI *api)
      : module_(M), thread_api_(api) {}

  void analyzeModule();

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

  std::vector<MPIOperation> getOperationsByKind(MPIOpKind kind) const;

  bool canCommunicate(const MPIOperation &op1, const MPIOperation &op2) const;
  MPICommunicationMatch
  classifyCommunicationMatch(const MPIOperation &op1,
                             const MPIOperation &op2) const;

  std::vector<NonBlockingOp> findOrphanedNonBlockingOps() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findPotentialDeadlocks() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findTagMismatches() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findCountDatatypeMismatches() const;

  std::vector<const llvm::Instruction *> findRankOutOfBounds() const;

  std::vector<RequestID> findPersistentRequestLeaks() const;

  std::vector<const llvm::Instruction *> findCancelWithoutWait() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findBufferOverlaps() const;

  std::vector<const llvm::Instruction *> findWildcardInCollective() const;

  std::vector<const llvm::Instruction *> findInPlaceConflicts() const;

  std::vector<const llvm::Instruction *> findNullHandles() const;

  std::vector<const llvm::Instruction *> findNegativeRoot() const;

  std::vector<const llvm::Instruction *> findInvalidTags() const;

  std::vector<const llvm::Instruction *> findInvalidRanks() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findTypeSizeMismatches() const;

  std::vector<const llvm::Instruction *> findDestroyNullComm() const;

  std::vector<const llvm::Instruction *> findRequestFreeAfterWait() const;

  std::vector<const llvm::Instruction *> findInPlaceWrongOp() const;

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
  mutable size_t next_communicator_subgroup_id_ = 1;
  std::unordered_map<const llvm::Value *, size_t> communicator_subgroup_ids_;
  std::unique_ptr<MPI::MPIRankAnalysis> rank_analysis_;

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
  void registerCommunicatorSubgroup(const llvm::Value *alias,
                                    const llvm::Value *root,
                                    int subgroup_token);
  size_t assignCommunicatorClass(CommunicatorID canonical);
  size_t getCommunicatorSubgroupID(const llvm::Value *communicator) const;
  void annotateRankConstraints(MPIOperation &op) const;
  int64_t getDatatypeExtent(const llvm::Value *datatype_arg,
                            const llvm::Instruction *context) const;
  void matchNonBlockingOps();
  std::unordered_map<std::string, size_t> deferred_lowering_stats_;
};

} // namespace mpi

#endif // MPI_PROCESS_MODEL_H
