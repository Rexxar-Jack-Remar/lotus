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

#include "Analysis/Concurrency/MPI/MPINormalization.h"
#include "Analysis/Concurrency/MPI/MPIOperation.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/MPI/MPISemanticEvent.h"
#include "Analysis/Concurrency/MPI/MPISemantics.h"
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

  std::vector<MPIOperation> &getMutableOperations() { return all_operations_; }

  const std::vector<MPIEvent> &getSemanticEvents() const {
    return semantic_events_;
  }

  std::vector<MPIEvent> &getMutableSemanticEvents() { return semantic_events_; }

  const std::vector<MPIPointToPointObligation> &
  getPointToPointObligations() const {
    return point_to_point_obligations_;
  }

  const std::map<RequestID, MPIRequestStateSummary> &
  getRequestStateSummaries() const {
    return request_state_summaries_;
  }

  const std::unordered_map<MPIOpKind, size_t> &getOperationKindCounts() const {
    return operation_kind_counts_;
  }

  const llvm::Module &getModule() const { return module_; }

  const std::unordered_map<std::string, size_t> &
  getDeferredLoweringStats() const {
    return deferred_lowering_stats_;
  }

  const std::unordered_map<NormalizationConfidence, size_t> &
  getNormalizationConfidenceCounts() const {
    return normalization_confidence_counts_;
  }

  bool hasInitThreadLevel() const { return has_init_thread_level_; }

  int getRequiredInitThreadLevel() const { return init_thread_required_level_; }

  bool hasProvidedInitThreadLevel() const {
    return has_provided_init_thread_level_;
  }

  int getProvidedInitThreadLevel() const { return init_thread_provided_level_; }

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
  std::map<RequestID, MPIRequestStateSummary> request_state_summaries_;
  std::vector<MPIEvent> semantic_events_;
  std::vector<MPIPointToPointObligation> point_to_point_obligations_;
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
  bool tryGetScalarRange(const llvm::Value *value, int &min_out,
                         int &max_out) const;
  void extractPointToPointDetails(MPIOperation &op, const llvm::CallBase *cb,
                                  const MPISemanticDescriptor &descriptor);
  void extractSendrecvDetails(MPIOperation &op, const llvm::CallBase *cb) const;
  void extractProbeDetails(MPIOperation &op, const llvm::CallBase *cb,
                           const MPISemanticDescriptor &descriptor) const;
  void extractCollectiveDetails(MPIOperation &op, const llvm::CallBase *cb,
                                llvm::StringRef semantic_tag,
                                const MPISemanticDescriptor &descriptor) const;
  void extractRequestDetails(MPIOperation &op, const llvm::CallBase *cb,
                             const MPISemanticDescriptor &descriptor) const;
  void extractRMAWindowDetails(MPIOperation &op, const llvm::CallBase *cb,
                               llvm::StringRef semantic_tag,
                               const MPISemanticDescriptor &descriptor) const;
  void extractRMADataDetails(MPIOperation &op, const llvm::CallBase *cb,
                             llvm::StringRef semantic_tag,
                             const MPISemanticDescriptor &descriptor) const;
  void extractRMASyncDetails(MPIOperation &op, const llvm::CallBase *cb,
                             llvm::StringRef semantic_tag,
                             const MPISemanticDescriptor &descriptor) const;
  void extractDatatypeDetails(MPIOperation &op, const llvm::CallBase *cb,
                              llvm::StringRef semantic_tag);
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
  const llvm::Value *
  canonicalizeDatatypeHandle(const llvm::Value *handle) const;
  void registerDatatypeExtent(const llvm::Value *handle, int64_t extent);
  void buildSemanticEvents();
  void buildPointToPointObligations();
  void analyzeRequestStateDomain();
  std::unordered_map<std::string, size_t> deferred_lowering_stats_;
  std::unordered_map<const llvm::Value *, int64_t> datatype_extent_bytes_;
  std::unordered_map<NormalizationConfidence, size_t>
      normalization_confidence_counts_;
  int init_thread_required_level_ = -1;
  bool has_init_thread_level_ = false;
  int init_thread_provided_level_ = -1;
  bool has_provided_init_thread_level_ = false;
};

} // namespace mpi

#endif // MPI_PROCESS_MODEL_H
