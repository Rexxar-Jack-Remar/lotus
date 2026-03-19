#include "Analysis/Concurrency/MPI/MPIAbstractState.h"

#include "Analysis/Concurrency/MPI/MPICollectiveAnalysis.h"
#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/MPI/MPIRMAAnalysis.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;

namespace mpi {

void MPIAbstractState::clear() {
  communicator_fact_by_class.clear();
  request_fact_by_request.clear();
  channel_automata_by_key.clear();
  collective_state_by_scope.clear();
  rma_epoch_by_key.clear();
  protocol_diagnostics.clear();

  communicator_facts.clear();
  function_summaries.clear();
  request_facts.clear();
  channel_automata.clear();
  collective_protocol_states.clear();
  rma_epoch_facts.clear();
  participant_sets.clear();
  channel_obligations.clear();
  protocol_frontiers.clear();
  rma_synchronization_facts.clear();
  model_gaps.clear();
  potential_deadlocks.clear();
  mismatched_collective_insts.clear();
  conditional_collective_insts.clear();
  wrong_root_inst_pairs.clear();
  unsynchronized_rma_insts.clear();
  rma_race_insts.clear();
  leaked_windows.clear();
  invalid_epoch_transitions.clear();
  use_after_free_windows.clear();
  double_window_free.clear();
  tracked_window_count = 0;
}

MPICommunicatorFact &
MPIAbstractState::upsertCommunicatorFact(size_t communicator_class_id) {
  return communicator_fact_by_class[communicator_class_id];
}

MPIRequestFact &MPIAbstractState::upsertRequestFact(RequestID request) {
  return request_fact_by_request[request];
}

MPIChannelAutomaton &
MPIAbstractState::upsertChannelAutomaton(const std::string &key) {
  return channel_automata_by_key[key];
}

MPICollectiveProtocolState &MPIAbstractState::upsertCollectiveState(
    size_t communicator_class_id, size_t communicator_subgroup_id,
    size_t participant_class_id, size_t protocol_class_id,
    size_t protocol_position) {
  return collective_state_by_scope[std::make_tuple(
      communicator_class_id, communicator_subgroup_id, participant_class_id,
      protocol_class_id, protocol_position)];
}

MPIRMAEpochFact &MPIAbstractState::upsertRMAEpochFact(
    size_t participant_class_id, WindowID window, size_t epoch_id) {
  return rma_epoch_by_key[std::make_tuple(participant_class_id, window, epoch_id)];
}

namespace {

bool isSendOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::SEND_BLOCKING || kind == MPIOpKind::SEND_NONBLOCKING;
}

bool isRecvOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::RECV_BLOCKING || kind == MPIOpKind::RECV_NONBLOCKING;
}

bool isCollectiveOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::BARRIER_BLOCKING ||
         kind == MPIOpKind::BARRIER_NONBLOCKING ||
         kind == MPIOpKind::COLLECTIVE_BLOCKING ||
         kind == MPIOpKind::COLLECTIVE_NONBLOCKING;
}

bool isRMAOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::RMA_WINDOW || kind == MPIOpKind::RMA_DATA ||
         kind == MPIOpKind::RMA_SYNC;
}

bool isBlockingPointToPointKind(MPIOpKind kind) {
  return kind == MPIOpKind::SEND_BLOCKING || kind == MPIOpKind::RECV_BLOCKING;
}

bool isMPIWildcardValue(int value) { return value == -1 || value == -2; }

bool rangesOverlap(int lhs_min, int lhs_max, int rhs_min, int rhs_max) {
  if (lhs_min < 0 || lhs_max < 0 || rhs_min < 0 || rhs_max < 0) {
    return true;
  }
  return !(lhs_max < rhs_min || rhs_max < lhs_min);
}

std::string classifyTag(int tag) {
  if (tag < 0) {
    return "wildcard";
  }
  return std::to_string(tag);
}

int64_t classifyDatatypeCountSize(const MPIOperation &op) {
  if (op.byte_length > 0) {
    return op.byte_length;
  }
  return op.datatype_size;
}

std::string buildChannelKey(const MPIOperation &send, const MPIOperation &recv) {
  return std::to_string(send.communicator_class_id != 0
                            ? send.communicator_class_id
                            : recv.communicator_class_id) +
         ":" + send.participant_set.toKey() + ":" + recv.participant_set.toKey() +
         ":" + classifyTag(send.tag >= 0 ? send.tag : recv.tag) + ":" +
         std::to_string(static_cast<int>(send.send_mode)) + ":" +
         std::to_string(std::max(classifyDatatypeCountSize(send),
                                 classifyDatatypeCountSize(recv)));
}

const Function *getInstructionCallee(const Instruction *inst) {
  const auto *cb = dyn_cast_or_null<CallBase>(inst);
  return cb ? cb->getCalledFunction() : nullptr;
}

std::vector<const Function *> collectRootFunctions(Module &module) {
  std::vector<const Function *> roots;
  std::set<const Function *> called_functions;

  for (const Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (const Instruction &inst : instructions(function)) {
      const Function *callee = getInstructionCallee(&inst);
      if (callee && !callee->isDeclaration()) {
        called_functions.insert(callee);
      }
    }
  }

  for (const Function &function : module) {
    if (!function.isDeclaration() && called_functions.count(&function) == 0) {
      roots.push_back(&function);
    }
  }
  if (roots.empty()) {
    for (const Function &function : module) {
      if (!function.isDeclaration()) {
        roots.push_back(&function);
      }
    }
  }
  return roots;
}

bool communicatorsCompatible(const MPICollectiveAnalysis::CollectiveCall &lhs,
                             const MPICollectiveAnalysis::CollectiveCall &rhs) {
  if (lhs.communicator_class_id != 0 && rhs.communicator_class_id != 0) {
    return lhs.communicator_class_id == rhs.communicator_class_id;
  }
  return lhs.comm && rhs.comm && lhs.comm == rhs.comm;
}

bool areCollectivesCompatible(const MPICollectiveAnalysis::CollectiveCall &lhs,
                              const MPICollectiveAnalysis::CollectiveCall &rhs) {
  if (!communicatorsCompatible(lhs, rhs)) {
    return true;
  }
  if (lhs.type != rhs.type) {
    return false;
  }
  if (lhs.root_rank >= 0 && rhs.root_rank >= 0 && lhs.root_rank != rhs.root_rank) {
    return false;
  }
  if (lhs.count >= 0 && rhs.count >= 0 && lhs.count != rhs.count) {
    return false;
  }
  if (lhs.recv_count >= 0 && rhs.recv_count >= 0 &&
      lhs.recv_count != rhs.recv_count) {
    return false;
  }
  if (lhs.datatype >= 0 && rhs.datatype >= 0 && lhs.datatype != rhs.datatype) {
    return false;
  }
  if (lhs.recv_datatype >= 0 && rhs.recv_datatype >= 0 &&
      lhs.recv_datatype != rhs.recv_datatype) {
    return false;
  }
  if (lhs.reduction_op >= 0 && rhs.reduction_op >= 0 &&
      lhs.reduction_op != rhs.reduction_op) {
    return false;
  }
  return lhs.in_place == rhs.in_place;
}

bool participantsMayOverlap(const MPIParticipantSet &lhs,
                            const MPIParticipantSet &rhs) {
  return lhs.unknown || rhs.unknown || lhs.mayOverlap(rhs);
}

bool isRMAWriteAccess(MPIRMAAccessKind kind) {
  return kind == MPIRMAAccessKind::Put || kind == MPIRMAAccessKind::Accumulate ||
         kind == MPIRMAAccessKind::Atomic;
}

} // namespace

MPIAbstractState MPIAbstractStateBuilder::build() const {
  MPIAbstractState state;
  state.participant_sets = process_model_.getParticipantSets();
  state.model_gaps = process_model_.getModelGaps();

  for (const MPICommunicatorFact &fact : process_model_.getCommunicatorFacts()) {
    MPICommunicatorFact &slot = state.upsertCommunicatorFact(fact.communicator_class_id);
    slot = fact;
  }

  const auto &operations = process_model_.getAllOperations();
  const auto &events = process_model_.getSemanticEvents();
  const auto &request_summaries = process_model_.getRequestStateSummaries();
  const auto &protocol_relations = collective_analysis_.getProtocolRelations();
  const auto &rma_relations = rma_analysis_.getSynchronizationRelations();
  const auto &rma_facts = rma_analysis_.getSynchronizationFacts();
  const MPI::MPIRankAnalysis *rank_analysis = process_model_.getRankAnalysis();

  std::unordered_map<const Instruction *, size_t> op_index_by_inst;
  std::unordered_map<const Instruction *, const MPIEvent *> event_by_inst;
  std::unordered_map<const Instruction *, MPICollectiveAnalysis::CollectiveCall>
      collective_call_by_inst;
  std::unordered_map<const Instruction *, MPIRMAAnalysis::RMAOperation>
      rma_relation_by_inst;
  std::unordered_map<const Instruction *, RMASynchronizationFact> rma_fact_by_inst;

  for (size_t idx = 0; idx < operations.size(); ++idx) {
    if (operations[idx].inst) {
      op_index_by_inst[operations[idx].inst] = idx;
    }
  }
  for (const MPIEvent &event : events) {
    if (event.inst) {
      event_by_inst[event.inst] = &event;
    }
  }
  for (const auto &call : protocol_relations) {
    if (call.inst) {
      collective_call_by_inst[call.inst] = call;
    }
  }
  for (const auto &relation : rma_relations) {
    if (relation.inst && !rma_relation_by_inst.count(relation.inst)) {
      rma_relation_by_inst[relation.inst] = relation;
    }
  }
  for (const auto &fact : rma_facts) {
    if (fact.inst && !rma_fact_by_inst.count(fact.inst)) {
      rma_fact_by_inst[fact.inst] = fact;
    }
  }

  std::unordered_map<const Instruction *, std::vector<size_t>> operations_by_instruction;
  for (size_t idx = 0; idx < operations.size(); ++idx) {
    if (operations[idx].inst) {
      operations_by_instruction[operations[idx].inst].push_back(idx);
    }
  }

  std::unordered_map<const Function *, size_t> summary_index_by_function;
  for (Function &function : module_) {
    if (function.isDeclaration()) {
      continue;
    }
    MPIFunctionSummary summary;
    summary.function = &function;
    for (const Instruction &inst : instructions(function)) {
      auto op_it = operations_by_instruction.find(&inst);
      if (op_it != operations_by_instruction.end()) {
        for (size_t op_index : op_it->second) {
          summary.direct_operation_indices.push_back(op_index);
          const MPIOperation &op = operations[op_index];
          if (isRMAOperationKind(op.kind)) {
            summary.rma_operation_indices.push_back(op_index);
          }
          if (op.kind == MPIOpKind::WAIT || op.kind == MPIOpKind::TEST ||
              op.request) {
            summary.request_operation_indices.push_back(op_index);
          }
          if (isSendOperationKind(op.kind) || isRecvOperationKind(op.kind)) {
            summary.channel_operation_indices.push_back(op_index);
          }
          if (isCollectiveOperationKind(op.kind)) {
            summary.collective_operation_indices.push_back(op_index);
          }
          if (op.communicator_class_id != 0 &&
              std::find(summary.communicator_class_ids.begin(),
                        summary.communicator_class_ids.end(),
                        op.communicator_class_id) ==
                  summary.communicator_class_ids.end()) {
            summary.communicator_class_ids.push_back(op.communicator_class_id);
          }
        }
      }
      const Function *callee = getInstructionCallee(&inst);
      if (callee && !callee->isDeclaration() &&
          std::find(summary.callees.begin(), summary.callees.end(), callee) ==
              summary.callees.end()) {
        summary.callees.push_back(callee);
      }
    }
    summary.expanded_operation_indices = summary.direct_operation_indices;
    summary.reaches_fixed_point = summary.callees.empty();
    state.function_summaries.push_back(summary);
    summary_index_by_function.emplace(&function, state.function_summaries.size() - 1);
  }

  std::vector<const Function *> roots = collectRootFunctions(module_);
  bool changed = true;
  size_t iteration = 0;
  while (changed && iteration < state.function_summaries.size() + 2) {
    changed = false;
    ++iteration;
    for (MPIFunctionSummary &summary : state.function_summaries) {
      std::vector<size_t> expanded;
      std::set<const Function *> seen_callees;
      summary.recursive = false;

      for (const Instruction &inst : instructions(*summary.function)) {
        auto op_it = operations_by_instruction.find(&inst);
        if (op_it != operations_by_instruction.end()) {
          expanded.insert(expanded.end(), op_it->second.begin(), op_it->second.end());
        }

        const Function *callee = getInstructionCallee(&inst);
        if (!callee || callee->isDeclaration()) {
          if (isa<CallBase>(&inst) && !getInstructionCallee(&inst)) {
            MPIModelGap gap;
            gap.domain = MPIModelGapDomain::Unknown;
            gap.inst = &inst;
            gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
            gap.relation.proof = concurrency::ProofStrength::Unknown;
            gap.relation.reason = "mpi_summary_indirect_call";
            gap.code = "mpi_summary_indirect_call";
            gap.detail = summary.function ? summary.function->getName().str() : "";
            state.model_gaps.push_back(gap);
          }
          continue;
        }
        if (callee == summary.function) {
          summary.recursive = true;
          continue;
        }
        auto callee_it = summary_index_by_function.find(callee);
        if (callee_it == summary_index_by_function.end() ||
            !seen_callees.insert(callee).second) {
          continue;
        }
        const MPIFunctionSummary &callee_summary =
            state.function_summaries[callee_it->second];
        expanded.insert(expanded.end(), callee_summary.expanded_operation_indices.begin(),
                        callee_summary.expanded_operation_indices.end());
      }

      if (expanded != summary.expanded_operation_indices) {
        summary.expanded_operation_indices = std::move(expanded);
        changed = true;
      }
      summary.iterations = iteration;
    }
  }

  for (MPIFunctionSummary &summary : state.function_summaries) {
    summary.reaches_fixed_point = true;
  }
  (void)roots;

  for (const auto &entry : request_summaries) {
    const MPIRequestStateSummary &summary = entry.second;
    MPIRequestFact &fact = state.upsertRequestFact(summary.request);
    fact.request = summary.request;
    fact.kind = summary.is_persistent
                    ? MPIRequestFactKind::Persistent
                    : (summary.is_collective ? MPIRequestFactKind::Collective
                                             : MPIRequestFactKind::PointToPoint);
    fact.origin_inst = summary.origin_inst;
    fact.activation_inst = summary.activation_inst;
    fact.last_transition_inst = summary.last_transition_inst;
    fact.state = summary.state;
    fact.is_persistent = summary.is_persistent;
    fact.is_collective = summary.is_collective;
    fact.peer_rank = summary.peer_rank;
    fact.tag = summary.tag;
    fact.communicator = summary.communicator;
    fact.communicator_class_id =
        process_model_.getCommunicatorClassID(summary.communicator);
    fact.send_mode = summary.send_mode;
    fact.relation.kind = summary.state == MPIRequestState::MustComplete
                             ? concurrency::RelationKind::MPIRequestCompletion
                             : concurrency::RelationKind::UnknownDueToModelGap;
    fact.relation.proof =
        summary.state == MPIRequestState::MustComplete
            ? concurrency::ProofStrength::Must
            : concurrency::ProofStrength::Unknown;
    fact.relation.reason = summary.state == MPIRequestState::MustComplete
                               ? "mpi_request_fact_complete"
                               : "mpi_request_fact_state";
  }

  for (const auto &entry : state.request_fact_by_request) {
    state.request_facts.push_back(entry.second);
  }

  for (size_t lhs = 0; lhs < operations.size(); ++lhs) {
    const MPIOperation &op1 = operations[lhs];
    if (!isSendOperationKind(op1.kind) && !isRecvOperationKind(op1.kind)) {
      continue;
    }
    for (size_t rhs = lhs + 1; rhs < operations.size(); ++rhs) {
      const MPIOperation &op2 = operations[rhs];
      if ((!isSendOperationKind(op1.kind) && !isRecvOperationKind(op2.kind)) ||
          (!isRecvOperationKind(op1.kind) && !isSendOperationKind(op2.kind))) {
        if (!((isSendOperationKind(op1.kind) && isRecvOperationKind(op2.kind)) ||
              (isRecvOperationKind(op1.kind) && isSendOperationKind(op2.kind)))) {
          continue;
        }
      }

      MPICommunicationMatch match = process_model_.classifyCommunicationMatch(op1, op2);
      if (match == MPICommunicationMatch::NoMatch) {
        continue;
      }

      const MPIOperation &send = isSendOperationKind(op1.kind) ? op1 : op2;
      const MPIOperation &recv = isSendOperationKind(op1.kind) ? op2 : op1;

      MPIChannelObligation channel;
      channel.lhs_operation_index = lhs;
      channel.rhs_operation_index = rhs;
      channel.sender_operation_index = isSendOperationKind(op1.kind) ? lhs : rhs;
      channel.receiver_operation_index = isSendOperationKind(op1.kind) ? rhs : lhs;
      channel.lhs_inst = op1.inst;
      channel.rhs_inst = op2.inst;
      channel.sender_inst = send.inst;
      channel.receiver_inst = recv.inst;
      channel.communicator_class_id = send.communicator_class_id != 0
                                          ? send.communicator_class_id
                                          : recv.communicator_class_id;
      channel.sender_set = send.participant_set;
      channel.receiver_set = recv.participant_set;
      channel.tag = send.tag >= 0 ? send.tag : recv.tag;
      channel.send_datatype_size = send.datatype_size;
      channel.recv_datatype_size = recv.datatype_size;
      channel.send_mode = send.send_mode;
      channel.request = send.request ? send.request : recv.request;
      channel.sender_request = send.request;
      channel.receiver_request = recv.request;
      channel.send_is_blocking = send.kind == MPIOpKind::SEND_BLOCKING;
      channel.recv_is_blocking = recv.kind == MPIOpKind::RECV_BLOCKING;
      channel.proof = match;
      channel.relation.kind =
          match == MPICommunicationMatch::MustMatch ||
                  match == MPICommunicationMatch::MayMatch
              ? concurrency::RelationKind::MatchedCommunication
              : concurrency::RelationKind::UnknownDueToModelGap;
      channel.relation.proof = match == MPICommunicationMatch::MustMatch
                                   ? concurrency::ProofStrength::Must
                                   : (match == MPICommunicationMatch::MayMatch
                                          ? concurrency::ProofStrength::May
                                          : concurrency::ProofStrength::Unknown);
      channel.relation.reason = "mpi_channel_automaton";

      auto maybeDischarge = [&](RequestID request) {
        auto request_it = state.request_fact_by_request.find(request);
        if (request_it == state.request_fact_by_request.end()) {
          return false;
        }
        if (request_it->second.state != MPIRequestState::MustComplete &&
            request_it->second.state != MPIRequestState::Freed &&
            request_it->second.state != MPIRequestState::Canceled) {
          return false;
        }
        const Instruction *transition_inst = request_it->second.last_transition_inst;
        auto op_it = op_index_by_inst.find(transition_inst);
        if (op_it != op_index_by_inst.end() &&
            operations[op_it->second].kind == MPIOpKind::WAIT) {
          return false;
        }
        channel.discharged = true;
        channel.discharge_inst = transition_inst;
        return true;
      };
      if (!maybeDischarge(channel.sender_request)) {
        maybeDischarge(channel.receiver_request);
      }

      state.channel_obligations.push_back(channel);
      MPIChannelAutomaton &automaton =
          state.upsertChannelAutomaton(buildChannelKey(send, recv));
      automaton.channel_class_id =
          send.channel_class_id != 0 ? send.channel_class_id : recv.channel_class_id;
      automaton.communicator_class_id = channel.communicator_class_id;
      automaton.sender_set = send.participant_set;
      automaton.receiver_set = recv.participant_set;
      automaton.tag = channel.tag;
      automaton.send_mode = send.send_mode;
      automaton.datatype_size_class =
          std::max(classifyDatatypeCountSize(send), classifyDatatypeCountSize(recv));
      automaton.has_wildcard_endpoint =
          channel.tag < 0 || send.participant_set.unknown || recv.participant_set.unknown;
      ++automaton.posted_receive_count;
      ++automaton.inflight_send_count;
      if (!channel.discharged) {
        ++automaton.unresolved_obligation_count;
      }
      automaton.obligations.push_back(channel);

      MPIChannelTransition send_transition;
      send_transition.operation_index = channel.sender_operation_index;
      send_transition.inst = send.inst;
      send_transition.is_send = true;
      send_transition.blocking = channel.send_is_blocking;
      send_transition.discharged = channel.discharged;
      send_transition.proof = channel.proof;
      send_transition.relation = channel.relation;
      automaton.transitions.push_back(send_transition);

      MPIChannelTransition recv_transition;
      recv_transition.operation_index = channel.receiver_operation_index;
      recv_transition.inst = recv.inst;
      recv_transition.is_recv = true;
      recv_transition.blocking = channel.recv_is_blocking;
      recv_transition.discharged = channel.discharged;
      recv_transition.proof = channel.proof;
      recv_transition.relation = channel.relation;
      automaton.transitions.push_back(recv_transition);

      if (match == MPICommunicationMatch::MayMatch ||
          match == MPICommunicationMatch::Unknown) {
        MPIModelGap gap;
        gap.domain = MPIModelGapDomain::PointToPoint;
        gap.inst = send.inst;
        gap.communicator = send.communicator;
        gap.communicator_class_id = channel.communicator_class_id;
        gap.participant_class_id = send.participant_class_id;
        gap.relation = channel.relation;
        gap.code = match == MPICommunicationMatch::Unknown ? "mpi_channel_unknown"
                                                           : "mpi_channel_partial";
        gap.detail = send.participant_set.toKey() + " -> " +
                     recv.participant_set.toKey();
        state.model_gaps.push_back(gap);
      }
    }
  }

  for (const auto &entry : state.channel_automata_by_key) {
    MPIChannelAutomaton automaton = entry.second;
    std::stable_sort(automaton.transitions.begin(), automaton.transitions.end(),
                     [&](const MPIChannelTransition &lhs,
                         const MPIChannelTransition &rhs) {
                       return lhs.operation_index < rhs.operation_index;
                     });
    state.channel_automata.push_back(std::move(automaton));
  }

  state.potential_deadlocks = process_model_.findPotentialDeadlocks();

  std::unordered_map<size_t, size_t> collective_position_by_op;
  for (const MPIFunctionSummary &summary : state.function_summaries) {
    size_t collective_position = 0;
    for (size_t op_index : summary.expanded_operation_indices) {
      if (op_index >= operations.size() || !isCollectiveOperationKind(operations[op_index].kind)) {
        continue;
      }
      collective_position_by_op.emplace(op_index, collective_position++);
    }
  }

  std::set<const Instruction *> seen_conditional_collectives;
  for (const MPIEvent &event : events) {
    if (!event.has_collective_semantics) {
      continue;
    }

    const MPIOperation &op = operations[event.operation_index];
    size_t protocol_position = collective_position_by_op.count(event.operation_index)
                                   ? collective_position_by_op[event.operation_index]
                                   : 0;
    MPICollectiveProtocolState &protocol_state = state.upsertCollectiveState(
        op.communicator_class_id, op.communicator_subgroup_id,
        op.participant_class_id, op.collective_protocol_class_id,
        protocol_position);
    protocol_state.communicator_class_id = op.communicator_class_id;
    protocol_state.communicator_subgroup_id = op.communicator_subgroup_id;
    protocol_state.participant_class_id = op.participant_class_id;
    protocol_state.protocol_class_id = op.collective_protocol_class_id;
    protocol_state.protocol_position = protocol_position;
    protocol_state.type = op.td_type;
    protocol_state.variant = op.collective_variant;
    protocol_state.shape = op.collective_shape;
    protocol_state.reachability = event.collective.reachability;
    protocol_state.operations.push_back(op.inst);
    protocol_state.relation.kind =
        concurrency::RelationKind::MPICollectiveParticipation;
    protocol_state.relation.proof =
        event.collective.reachability == ProtocolReachability::AllRanks
            ? concurrency::ProofStrength::Must
            : concurrency::ProofStrength::May;
    protocol_state.relation.reason = "mpi_collective_summary_position";
    state.protocol_diagnostics["collective_slots_tracked"]++;
    if (event.collective.reachability != ProtocolReachability::AllRanks) {
      state.protocol_diagnostics["collective_partial_reachability"]++;
    }

    bool conditional = event.collective.reachability == ProtocolReachability::SomeRanks;
    if (!conditional && event.collective.reachability == ProtocolReachability::Unknown &&
        rank_analysis) {
      if (rank_analysis->dependsOnRank(op.inst)) {
        conditional = true;
      }
      for (const BasicBlock *pred : predecessors(op.inst->getParent())) {
        const auto *br = dyn_cast<BranchInst>(pred->getTerminator());
        if (br && br->isConditional() &&
            rank_analysis->dependsOnRank(br->getCondition())) {
          conditional = true;
          break;
        }
      }
    }
    if (conditional && seen_conditional_collectives.insert(op.inst).second) {
      state.conditional_collective_insts.push_back(op.inst);
      state.protocol_diagnostics["collective_rank_filtered"]++;
    }
  }

  for (const auto &entry : state.collective_state_by_scope) {
    const auto &key = entry.first;
    MPICollectiveProtocolState protocol_state = entry.second;
    CollectiveProtocolFrontier frontier;
    frontier.communicator_class_id = std::get<0>(key);
    frontier.communicator_subgroup_id = std::get<1>(key);
    frontier.participant_class_id = std::get<2>(key);
    frontier.protocol_class_id = std::get<3>(key);
    frontier.frontier_position = std::get<4>(key);
    frontier.frontier_id = state.protocol_frontiers.size() + 1;
    frontier.relation.kind = concurrency::RelationKind::SameCollectiveFrontier;
    frontier.relation.proof = protocol_state.relation.proof;
    frontier.relation.reason = protocol_state.relation.reason;
    if (!protocol_state.operations.empty()) {
      const auto op_it = op_index_by_inst.find(protocol_state.operations.front());
      if (op_it != op_index_by_inst.end()) {
        frontier.participants = operations[op_it->second].participant_set;
      }
    }
    frontier.transitions = protocol_state.operations;
    if (protocol_state.reachability != ProtocolReachability::AllRanks) {
      frontier.diagnostics.push_back("mpi_collective_frontier_partial_participants");
      frontier.relation.proof = concurrency::ProofStrength::May;
    }
    state.protocol_frontiers.push_back(frontier);
    state.collective_protocol_states.push_back(std::move(protocol_state));
  }

  std::map<std::tuple<size_t, size_t, size_t, size_t>,
           std::vector<const Instruction *>>
      collectives_by_frontier;
  for (const CollectiveProtocolFrontier &frontier : state.protocol_frontiers) {
    collectives_by_frontier[std::make_tuple(frontier.communicator_class_id,
                                            frontier.communicator_subgroup_id,
                                            frontier.protocol_class_id,
                                            frontier.frontier_position)] =
        frontier.transitions;
  }

  for (const auto &entry : collectives_by_frontier) {
    std::vector<MPICollectiveAnalysis::CollectiveCall> calls;
    for (const Instruction *inst : entry.second) {
      auto it = collective_call_by_inst.find(inst);
      if (it != collective_call_by_inst.end()) {
        calls.push_back(it->second);
      }
    }
    for (size_t i = 0; i < calls.size(); ++i) {
      for (size_t j = i + 1; j < calls.size(); ++j) {
        if (!areCollectivesCompatible(calls[i], calls[j])) {
          state.mismatched_collective_insts.emplace_back(calls[i].inst, calls[j].inst);
          state.protocol_diagnostics["collective_mismatch_pairs"]++;
        }
        if (calls[i].type == calls[j].type && calls[i].root_rank >= 0 &&
            calls[j].root_rank >= 0 && calls[i].root_rank != calls[j].root_rank) {
          state.wrong_root_inst_pairs.emplace_back(calls[i].inst, calls[j].inst);
        }
      }
    }
  }

  state.rma_synchronization_facts = rma_facts;
  state.invalid_epoch_transitions = rma_analysis_.findInvalidEpochTransitions();
  state.use_after_free_windows = rma_analysis_.findUseAfterFreeWindows();
  state.double_window_free = rma_analysis_.findDoubleWindowFree();
  state.leaked_windows = rma_analysis_.findLeakedWindows();

  std::set<WindowID> tracked_windows;
  for (const auto &relation : rma_relations) {
    if (relation.window) {
      tracked_windows.insert(relation.window);
    }
  }
  state.tracked_window_count = tracked_windows.size();

  for (const RMASynchronizationFact &fact : rma_facts) {
    MPIRMAEpochFact &epoch_fact =
        state.upsertRMAEpochFact(fact.participant_class_id, fact.window, fact.epoch_id);
    epoch_fact.window = fact.window;
    epoch_fact.epoch_id = fact.epoch_id;
    epoch_fact.participant_class_id = fact.participant_class_id;
    epoch_fact.participants = fact.participants;
    epoch_fact.sync_kind = fact.sync_kind;
    epoch_fact.completion = fact.completion;
    epoch_fact.relation = fact.relation;
    auto relation_it = rma_relation_by_inst.find(fact.inst);
    if (relation_it != rma_relation_by_inst.end()) {
      epoch_fact.sync_model = relation_it->second.sync_model == MPIRMAAnalysis::SyncModel::FENCE
                                  ? MPIRMASyncModel::Fence
                                  : (relation_it->second.sync_model ==
                                             MPIRMAAnalysis::SyncModel::LOCK_UNLOCK
                                         ? MPIRMASyncModel::LockUnlock
                                         : (relation_it->second.sync_model ==
                                                    MPIRMAAnalysis::SyncModel::PSCW
                                                ? MPIRMASyncModel::PSCW
                                                : MPIRMASyncModel::None));
      epoch_fact.sync_start = relation_it->second.sync_start;
      epoch_fact.sync_end = relation_it->second.sync_end;
    }
    epoch_fact.operations.push_back(fact.inst);
  }
  for (const auto &entry : state.rma_epoch_by_key) {
    state.rma_epoch_facts.push_back(entry.second);
  }

  std::set<const Instruction *> synchronized_rma;
  for (const RMASynchronizationFact &fact : rma_facts) {
    if (fact.access_kind == MPIRMAAccessKind::None) {
      continue;
    }
    if (fact.relation.kind == concurrency::RelationKind::SameSynchronizationEpoch ||
        fact.relation.kind ==
            concurrency::RelationKind::LocalOnlySynchronizationCompletion) {
      synchronized_rma.insert(fact.inst);
    }
  }
  for (const auto &relation : rma_relations) {
    if (!synchronized_rma.count(relation.inst) ||
        relation.sync_model == MPIRMAAnalysis::SyncModel::NONE) {
      state.unsynchronized_rma_insts.push_back(relation.inst);
    }
  }

  ThreadAPI *thread_api = ThreadAPI::getThreadAPI();
  auto raceFactsConflict = [&](const MPIRMAAnalysis::RMAOperation &lhs,
                               const MPIRMAAnalysis::RMAOperation &rhs) {
    auto lhs_fact_it = rma_fact_by_inst.find(lhs.inst);
    auto rhs_fact_it = rma_fact_by_inst.find(rhs.inst);
    const RMASynchronizationFact *lhs_fact =
        lhs_fact_it == rma_fact_by_inst.end() ? nullptr : &lhs_fact_it->second;
    const RMASynchronizationFact *rhs_fact =
        rhs_fact_it == rma_fact_by_inst.end() ? nullptr : &rhs_fact_it->second;

    if (lhs.window != rhs.window) {
      return false;
    }
    if (lhs_fact && rhs_fact &&
        !participantsMayOverlap(lhs_fact->participants, rhs_fact->participants)) {
      return false;
    }

    const int lhs_target = lhs_fact ? lhs_fact->target_rank : lhs.target_rank;
    const int rhs_target = rhs_fact ? rhs_fact->target_rank : rhs.target_rank;
    const int lhs_target_min = lhs_fact ? lhs_fact->target_rank_min : lhs.target_rank_min;
    const int lhs_target_max = lhs_fact ? lhs_fact->target_rank_max : lhs.target_rank_max;
    const int rhs_target_min = rhs_fact ? rhs_fact->target_rank_min : rhs.target_rank_min;
    const int rhs_target_max = rhs_fact ? rhs_fact->target_rank_max : rhs.target_rank_max;
    if (!rangesOverlap(lhs_target, lhs_target, rhs_target, rhs_target) &&
        !rangesOverlap(lhs_target_min, lhs_target_max, rhs_target_min,
                       rhs_target_max)) {
      return false;
    }

    const int64_t lhs_disp = lhs_fact ? lhs_fact->target_disp : lhs.target_disp;
    const int64_t rhs_disp = rhs_fact ? rhs_fact->target_disp : rhs.target_disp;
    const int64_t lhs_len =
        lhs_fact && lhs_fact->byte_length > 0
            ? lhs_fact->byte_length
            : (lhs.byte_length > 0 ? lhs.byte_length : 1);
    const int64_t rhs_len =
        rhs_fact && rhs_fact->byte_length > 0
            ? rhs_fact->byte_length
            : (rhs.byte_length > 0 ? rhs.byte_length : 1);
    if (lhs_disp != -1 && rhs_disp != -1) {
      int64_t lhs_end = lhs_disp + lhs_len;
      int64_t rhs_end = rhs_disp + rhs_len;
      if (!(lhs_disp < rhs_end && rhs_disp < lhs_end)) {
        return false;
      }
    }

    const Function *lhs_callee = thread_api->getCallee(lhs.inst);
    const Function *rhs_callee = thread_api->getCallee(rhs.inst);
    ThreadAPI::TD_TYPE lhs_type =
        lhs_callee ? thread_api->getType(lhs_callee) : ThreadAPI::TD_DUMMY;
    ThreadAPI::TD_TYPE rhs_type =
        rhs_callee ? thread_api->getType(rhs_callee) : ThreadAPI::TD_DUMMY;
    MPIRMAAccessKind lhs_access =
        lhs_fact ? lhs_fact->access_kind
                 : (lhs_type == ThreadAPI::TD_MPI_GET ? MPIRMAAccessKind::Get
                                                      : MPIRMAAccessKind::Put);
    MPIRMAAccessKind rhs_access =
        rhs_fact ? rhs_fact->access_kind
                 : (rhs_type == ThreadAPI::TD_MPI_GET ? MPIRMAAccessKind::Get
                                                      : MPIRMAAccessKind::Put);
    if (!isRMAWriteAccess(lhs_access) && !isRMAWriteAccess(rhs_access)) {
      return false;
    }

    if (lhs_fact && rhs_fact) {
      if (lhs_fact->completion == MPIRMACompletionStrength::Remote &&
          rhs_fact->completion == MPIRMACompletionStrength::Remote &&
          lhs_fact->epoch_id != 0 && lhs_fact->epoch_id == rhs_fact->epoch_id) {
        return false;
      }
      if (lhs_fact->completion == MPIRMACompletionStrength::Local ||
          rhs_fact->completion == MPIRMACompletionStrength::Local) {
        return true;
      }
      if (lhs_fact->code == "mpi_rma_pscw_group_unresolved" ||
          rhs_fact->code == "mpi_rma_pscw_group_unresolved") {
        return true;
      }
    }

    if (lhs.sync_model == MPIRMAAnalysis::SyncModel::NONE ||
        rhs.sync_model == MPIRMAAnalysis::SyncModel::NONE) {
      return true;
    }
    if (lhs.sync_model != rhs.sync_model || lhs.local_completion_only ||
        rhs.local_completion_only) {
      return true;
    }
    return !(lhs.epoch_id != 0 && lhs.epoch_id == rhs.epoch_id);
  };

  for (size_t i = 0; i < rma_relations.size(); ++i) {
    for (size_t j = i + 1; j < rma_relations.size(); ++j) {
      if (raceFactsConflict(rma_relations[i], rma_relations[j])) {
        state.rma_race_insts.emplace_back(rma_relations[i].inst, rma_relations[j].inst);
      }
    }
  }

  for (const auto &entry : state.communicator_fact_by_class) {
    state.communicator_facts.push_back(entry.second);
  }

  return state;
}

} // namespace mpi
