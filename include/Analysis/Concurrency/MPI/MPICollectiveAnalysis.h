/**
 * @file MPICollectiveAnalysis.h
 * @brief MPI Collective Operation Analysis
 *
 * This file provides analysis for MPI collective operations,
 * checking for mismatches and correctness.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_COLLECTIVE_ANALYSIS_H
#define MPI_COLLECTIVE_ANALYSIS_H

#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mpi {

class MPIProcessModel;

class MPICollectiveAnalysis {
public:
  struct CollectiveStateKey {
    size_t communicator_class_id = 0;
    size_t communicator_subgroup_id = 0;
    size_t collective_protocol_class_id = 0;

    bool operator<(const CollectiveStateKey &other) const {
      if (communicator_class_id != other.communicator_class_id) {
        return communicator_class_id < other.communicator_class_id;
      }
      if (communicator_subgroup_id != other.communicator_subgroup_id) {
        return communicator_subgroup_id < other.communicator_subgroup_id;
      }
      return collective_protocol_class_id < other.collective_protocol_class_id;
    }
  };

  struct CollectiveProtocolState {
    size_t next_slot = 0;
    ThreadAPI::TD_TYPE expected_type = ThreadAPI::TD_DUMMY;
    bool has_expected_type = false;
  };

  struct CollectiveCall {
    const llvm::Instruction *inst;
    ThreadAPI::TD_TYPE type;
    CommunicatorID comm;
    size_t communicator_class_id = 0;
    size_t communicator_subgroup_id = 0;
    size_t collective_protocol_class_id = 0;
    const llvm::Function *function;
    size_t sequence_index = 0;
    size_t protocol_sequence_id = 0;
    ProtocolReachability reachability = ProtocolReachability::Unknown;
    concurrency::Relation protocol_relation;

    int root_rank = -1;
    int count = -1;
    int recv_count = -1;
    int datatype = -1;
    int recv_datatype = -1;
    int reduction_op = -1;
    bool in_place = false;
  };

  explicit MPICollectiveAnalysis(const MPIProcessModel &model)
      : process_model_(model) {}

  void analyzeCollectives();

  std::vector<std::pair<CollectiveCall, CollectiveCall>>
  findMismatchedCollectives() const;

  std::vector<const llvm::Instruction *> findConditionalCollectives() const;
  std::vector<std::pair<CollectiveCall, CollectiveCall>>
  findWrongRootRanks() const;
  const std::unordered_map<std::string, size_t> &
  getProtocolDiagnostics() const {
    return protocol_diagnostics_;
  }
  const std::vector<CollectiveCall> &getProtocolRelations() const {
    return collective_calls_;
  }
  size_t getProtocolRelationCount(concurrency::RelationKind kind) const;

private:
  const MPIProcessModel &process_model_;
  std::vector<CollectiveCall> collective_calls_;
  std::map<CollectiveStateKey, CollectiveProtocolState> protocol_states_;
  mutable std::unordered_map<std::string, size_t> protocol_diagnostics_;

  bool areCollectivesCompatible(const CollectiveCall &c1,
                                const CollectiveCall &c2) const;
  static int getRootArgIndex(ThreadAPI::TD_TYPE type);
};

} // namespace mpi

#endif // MPI_COLLECTIVE_ANALYSIS_H
