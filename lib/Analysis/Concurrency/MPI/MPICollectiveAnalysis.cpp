/**
 * @file MPICollectiveAnalysis.cpp
 * @brief MPI Collective Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPICollectiveAnalysis.h"

#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mpi {

namespace {

bool communicatorsMayAlias(CommunicatorID lhs, CommunicatorID rhs) {
  if (!lhs || !rhs) {
    return false;
  }
  if (lhs == rhs) {
    return true;
  }

  const auto *lhs_arg = dyn_cast<Argument>(lhs);
  const auto *rhs_arg = dyn_cast<Argument>(rhs);
  if (lhs_arg && rhs_arg && lhs_arg->getArgNo() == rhs_arg->getArgNo()) {
    return true;
  }
  return false;
}

using ProtocolScopeKey = std::tuple<size_t, size_t, size_t, size_t>;

ProtocolScopeKey
getProtocolScopeKey(const MPICollectiveAnalysis::CollectiveCall &call) {
  return std::make_tuple(
      call.communicator_class_id, call.communicator_subgroup_id,
      call.collective_protocol_class_id, call.protocol_sequence_id);
}

} // namespace

void MPICollectiveAnalysis::analyzeCollectives() {
  collective_calls_.clear();
  this->protocol_states_.clear();
  protocol_diagnostics_.clear();

  auto readConstArg = [](const CallBase *cb, int idx, int &out) {
    if (!cb || idx < 0 || static_cast<unsigned>(idx) >= cb->arg_size()) {
      return false;
    }
    const auto *ci = dyn_cast<ConstantInt>(cb->getArgOperand(idx));
    if (!ci) {
      return false;
    }
    out = ci->getSExtValue();
    return true;
  };

  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.kind == MPIOpKind::COLLECTIVE_BLOCKING ||
        op.kind == MPIOpKind::COLLECTIVE_NONBLOCKING ||
        op.kind == MPIOpKind::BARRIER_BLOCKING ||
        op.kind == MPIOpKind::BARRIER_NONBLOCKING) {
      CollectiveCall call;
      call.inst = op.inst;
      call.type = op.td_type;
      call.comm = op.communicator;
      call.communicator_class_id = op.communicator_class_id;
      call.communicator_subgroup_id = op.communicator_subgroup_id;
      call.collective_protocol_class_id = op.collective_protocol_class_id;
      call.function = op.function;
      call.sequence_index = op.protocol_sequence_id;
      call.protocol_sequence_id = op.protocol_sequence_id;
      call.reachability = op.protocol_reachability;
      call.protocol_relation.kind = concurrency::RelationKind::SameProtocolSlot;
      call.protocol_relation.proof =
          op.protocol_reachability == ProtocolReachability::AllRanks
              ? concurrency::ProofStrength::Must
              : concurrency::ProofStrength::May;
      call.protocol_relation.reason = "mpi_collective_protocol_automaton_slot";
      protocol_diagnostics_["collective_slots_tracked"]++;
      if (op.protocol_reachability != ProtocolReachability::AllRanks) {
        protocol_diagnostics_["collective_partial_reachability"]++;
      }

      if (op.td_type == ThreadAPI::TD_MPI_BCAST ||
          op.td_type == ThreadAPI::TD_MPI_REDUCE ||
          op.td_type == ThreadAPI::TD_MPI_GATHER ||
          op.td_type == ThreadAPI::TD_MPI_SCATTER) {
        const CallBase *CB = dyn_cast<CallBase>(op.inst);
        int root_arg = getRootArgIndex(op.td_type);
        if (CB && root_arg >= 0 &&
            static_cast<unsigned>(root_arg) < CB->arg_size()) {
          if (const ConstantInt *root =
                  dyn_cast<ConstantInt>(CB->getArgOperand(root_arg))) {
            call.root_rank = root->getSExtValue();
          }
        }
      }

      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb) {
        switch (op.td_type) {
        case ThreadAPI::TD_MPI_BCAST:
          readConstArg(cb, 1, call.count);
          readConstArg(cb, 2, call.datatype);
          break;
        case ThreadAPI::TD_MPI_REDUCE:
        case ThreadAPI::TD_MPI_ALLREDUCE:
          readConstArg(cb, 2, call.count);
          readConstArg(cb, 3, call.datatype);
          readConstArg(cb, 4, call.reduction_op);
          break;
        case ThreadAPI::TD_MPI_GATHER:
          readConstArg(cb, 1, call.count);
          readConstArg(cb, 2, call.datatype);
          readConstArg(cb, 4, call.recv_count);
          readConstArg(cb, 5, call.recv_datatype);
          call.in_place = cb->getArgOperand(0) == cb->getArgOperand(3);
          break;
        case ThreadAPI::TD_MPI_SCATTER:
          readConstArg(cb, 1, call.count);
          readConstArg(cb, 2, call.datatype);
          readConstArg(cb, 4, call.recv_count);
          readConstArg(cb, 5, call.recv_datatype);
          call.in_place = cb->getArgOperand(0) == cb->getArgOperand(3);
          break;
        case ThreadAPI::TD_MPI_ALLGATHER:
        case ThreadAPI::TD_MPI_ALLTOALL:
          readConstArg(cb, 1, call.count);
          readConstArg(cb, 2, call.datatype);
          readConstArg(cb, 4, call.recv_count);
          readConstArg(cb, 5, call.recv_datatype);
          break;
        case ThreadAPI::TD_MPI_REDUCE_SCATTER:
        case ThreadAPI::TD_MPI_SCAN:
          readConstArg(cb, 2, call.datatype);
          readConstArg(cb, 3, call.reduction_op);
          break;
        default:
          break;
        }
      }

      MPICollectiveAnalysis::CollectiveStateKey state_key;
      state_key.communicator_class_id = call.communicator_class_id;
      state_key.communicator_subgroup_id = call.communicator_subgroup_id;
      state_key.collective_protocol_class_id =
          call.collective_protocol_class_id;
      MPICollectiveAnalysis::CollectiveProtocolState &state =
          this->protocol_states_[state_key];
      if (state.next_slot != call.protocol_sequence_id) {
        protocol_diagnostics_["collective_automaton_slot_gap"]++;
      }
      if (!state.has_expected_type) {
        state.expected_type = call.type;
        state.has_expected_type = true;
      } else if (state.expected_type != call.type) {
        protocol_diagnostics_["collective_automaton_type_drift"]++;
      }
      state.next_slot = call.protocol_sequence_id + 1;

      collective_calls_.push_back(call);
    }
  }
}

size_t MPICollectiveAnalysis::getProtocolRelationCount(
    concurrency::RelationKind kind) const {
  size_t count = 0;
  for (const CollectiveCall &call : collective_calls_) {
    if (call.protocol_relation.kind == kind) {
      ++count;
    }
  }
  return count;
}

int MPICollectiveAnalysis::getRootArgIndex(ThreadAPI::TD_TYPE type) {
  switch (type) {
  case ThreadAPI::TD_MPI_BCAST:
    return 3;
  case ThreadAPI::TD_MPI_REDUCE:
    return 5;
  case ThreadAPI::TD_MPI_GATHER:
  case ThreadAPI::TD_MPI_SCATTER:
    return 6;
  default:
    return -1;
  }
}

bool MPICollectiveAnalysis::areCollectivesCompatible(
    const CollectiveCall &c1, const CollectiveCall &c2) const {
  if (!c1.comm || !c2.comm) {
    return true;
  }
  if (c1.communicator_class_id != 0 && c2.communicator_class_id != 0 &&
      c1.communicator_class_id == c2.communicator_class_id) {
  } else if (!communicatorsMayAlias(c1.comm, c2.comm)) {
    return true;
  }

  if (c1.type != c2.type)
    return false;

  if (c1.root_rank != -1 && c2.root_rank != -1 &&
      c1.root_rank != c2.root_rank) {
    return false;
  }
  if (c1.count != -1 && c2.count != -1 && c1.count != c2.count) {
    return false;
  }
  if (c1.recv_count != -1 && c2.recv_count != -1 &&
      c1.recv_count != c2.recv_count) {
    return false;
  }
  if (c1.datatype != -1 && c2.datatype != -1 && c1.datatype != c2.datatype) {
    return false;
  }
  if (c1.recv_datatype != -1 && c2.recv_datatype != -1 &&
      c1.recv_datatype != c2.recv_datatype) {
    return false;
  }
  if (c1.reduction_op != -1 && c2.reduction_op != -1 &&
      c1.reduction_op != c2.reduction_op) {
    return false;
  }
  if (c1.in_place != c2.in_place) {
    return false;
  }

  return true;
}

std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall,
                      MPICollectiveAnalysis::CollectiveCall>>
MPICollectiveAnalysis::findMismatchedCollectives() const {
  std::vector<std::pair<CollectiveCall, CollectiveCall>> mismatches;

  std::map<ProtocolScopeKey, std::vector<const CollectiveCall *>> by_scope;
  for (const CollectiveCall &call : collective_calls_) {
    by_scope[getProtocolScopeKey(call)].push_back(&call);
  }

  for (const auto &entry : by_scope) {
    const auto &calls = entry.second;
    for (size_t i = 0; i < calls.size(); ++i) {
      for (size_t j = i + 1; j < calls.size(); ++j) {
        const CollectiveCall &c1 = *calls[i];
        const CollectiveCall &c2 = *calls[j];

        if (!communicatorsMayAlias(c1.comm, c2.comm) &&
            !(c1.communicator_class_id != 0 &&
              c1.communicator_class_id == c2.communicator_class_id)) {
          continue;
        }

        if (!areCollectivesCompatible(c1, c2)) {
          protocol_diagnostics_["collective_mismatch_pairs"]++;
          mismatches.emplace_back(c1, c2);
        }
      }
    }
  }

  return mismatches;
}

std::vector<const Instruction *>
MPICollectiveAnalysis::findConditionalCollectives() const {
  std::vector<const Instruction *> conditional;
  MPI::MPIRankAnalysis rank_analysis(
      const_cast<Module &>(process_model_.getModule()));
  bool rank_analysis_ready = false;

  for (const CollectiveCall &call : collective_calls_) {
    if (call.reachability == ProtocolReachability::SomeRanks) {
      protocol_diagnostics_["collective_rank_filtered"]++;
      conditional.push_back(call.inst);
      continue;
    }
    if (call.reachability == ProtocolReachability::AllRanks) {
      continue;
    }

    if (!rank_analysis_ready) {
      rank_analysis.analyze();
      rank_analysis_ready = true;
    }

    const BasicBlock *BB = call.inst->getParent();

    MPI::RankExpr rank = rank_analysis.getRankAtInstruction(call.inst);
    if (rank.kind == MPI::RankExpr::Concrete ||
        rank.kind == MPI::RankExpr::Range) {
      protocol_diagnostics_["collective_rank_filtered"]++;
      conditional.push_back(call.inst);
      continue;
    }

    bool rank_guarded_predecessor = false;
    for (const BasicBlock *pred : predecessors(BB)) {
      const Instruction *term = pred->getTerminator();
      const auto *br = dyn_cast_or_null<BranchInst>(term);
      if (!br || !br->isConditional()) {
        continue;
      }
      if (rank_analysis.dependsOnRank(br->getCondition())) {
        rank_guarded_predecessor = true;
        break;
      }
    }

    if (rank_guarded_predecessor) {
      protocol_diagnostics_["collective_rank_guarded_branch"]++;
      conditional.push_back(call.inst);
    }
  }

  return conditional;
}

std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall,
                      MPICollectiveAnalysis::CollectiveCall>>
MPICollectiveAnalysis::findWrongRootRanks() const {
  std::vector<std::pair<CollectiveCall, CollectiveCall>> wrong_roots;

  std::map<ProtocolScopeKey, std::vector<const CollectiveCall *>> by_scope;
  for (const CollectiveCall &call : collective_calls_) {
    if (call.root_rank < 0) {
      continue;
    }
    by_scope[getProtocolScopeKey(call)].push_back(&call);
  }

  for (auto &entry : by_scope) {
    auto &calls = entry.second;
    for (size_t i = 0; i < calls.size(); ++i) {
      for (size_t j = i + 1; j < calls.size(); ++j) {
        if (calls[i]->type != calls[j]->type) {
          continue;
        }
        if (calls[i]->root_rank != calls[j]->root_rank) {
          wrong_roots.emplace_back(*calls[i], *calls[j]);
        }
      }
    }
  }

  return wrong_roots;
}

} // namespace mpi
