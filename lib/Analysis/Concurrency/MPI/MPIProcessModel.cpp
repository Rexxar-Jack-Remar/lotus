/**
 * @file MPIProcessModel.cpp
 * @brief MPI Process Model Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPIProcessModel.h"

#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/MPI/MPISemantics.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <set>
#include <string>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace mpi {

namespace {

const Value *getOperandBySignedIndex(const CallBase *cb, int index) {
  if (!cb) {
    return nullptr;
  }
  int resolved = index;
  if (resolved < 0) {
    resolved = static_cast<int>(cb->arg_size()) + resolved;
  }
  if (resolved < 0 || resolved >= static_cast<int>(cb->arg_size())) {
    return nullptr;
  }
  return cb->getArgOperand(static_cast<unsigned>(resolved));
}

bool isMPIWildcardValue(int value) { return value == -1 || value == -2; }

bool isMPIValidRankLikeValue(int value) {
  return value >= 0 || value == -1 || value == -2;
}

bool isLikelyNullHandle(const Value *value) {
  if (!value) {
    return true;
  }
  value = value->stripPointerCasts();
  if (isa<ConstantPointerNull>(value)) {
    return true;
  }
  if (!value->hasName()) {
    return false;
  }
  StringRef name = value->getName();
  return name.contains("MPI_REQUEST_NULL") || name.contains("MPI_COMM_NULL") ||
         name.contains("MPI_WIN_NULL") || name.contains("MPI_INFO_NULL");
}

bool isLikelyMPIInPlace(const Value *value) {
  if (!value) {
    return false;
  }
  value = value->stripPointerCasts();
  return value->hasName() && value->getName().contains("MPI_IN_PLACE");
}

bool disablesDeterministicMPIOrdering(int provided_level) {
  return provided_level >= 2;
}

bool rangesOverlap(int lhs_min, int lhs_max, int rhs_min, int rhs_max) {
  if (lhs_min < 0 || lhs_max < 0 || rhs_min < 0 || rhs_max < 0) {
    return true;
  }
  return !(lhs_max < rhs_min || rhs_max < lhs_min);
}

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

const Value *traceCommunicatorRoot(const Value *value) {
  if (!value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolved = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = current->stripPointerCasts();
    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }
    if (const auto *store = dyn_cast<StoreInst>(current)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }
    if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (const Value *underlying = getUnderlyingObject(current)) {
      current = underlying->stripPointerCasts();
    }

    if (!resolved) {
      resolved = current;
    } else if (resolved != current) {
      return nullptr;
    }
  }

  return resolved ? resolved : value->stripPointerCasts();
}

bool sameCommunicatorForProof(const MPIOperation &lhs,
                              const MPIOperation &rhs) {
  if (lhs.communicator_class_id != 0 && rhs.communicator_class_id != 0 &&
      lhs.communicator_class_id == rhs.communicator_class_id) {
    return true;
  }
  return communicatorsMayAlias(lhs.communicator, rhs.communicator);
}

const Value *canonicalMemoryBase(const Value *value) {
  if (!value) {
    return nullptr;
  }
  value = value->stripPointerCasts();
  if (const auto *gep = dyn_cast<GEPOperator>(value)) {
    value = gep->getPointerOperand()->stripPointerCasts();
  }
  if (const Value *underlying = getUnderlyingObject(value)) {
    value = underlying->stripPointerCasts();
  }
  return value;
}

bool isBeforeInBlock(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs->getParent() != rhs->getParent()) {
    return false;
  }
  for (const Instruction &inst : *lhs->getParent()) {
    if (&inst == lhs) {
      return true;
    }
    if (&inst == rhs) {
      return false;
    }
  }
  return false;
}

bool mayDefinitionReach(const Instruction *def, const Instruction *use) {
  if (!def || !use || def->getFunction() != use->getFunction()) {
    return false;
  }
  if (def->getParent() == use->getParent()) {
    return isBeforeInBlock(def, use);
  }

  Function *func = const_cast<Function *>(def->getFunction());
  DominatorTree DT(*func);
  return DT.dominates(def->getParent(), use->getParent());
}

bool isDirectStoreToLocation(const StoreInst *store, const Value *base) {
  if (!store || !base) {
    return false;
  }
  const Value *ptr = store->getPointerOperand()->stripPointerCasts();
  if (isa<GEPOperator>(ptr)) {
    return false;
  }
  return canonicalMemoryBase(ptr) == base;
}

bool getIndexedStoreTarget(const StoreInst *store, const Value *base,
                           uint64_t &index_out) {
  if (!store || !base) {
    return false;
  }
  const Value *ptr = store->getPointerOperand();
  while (const auto *cast = dyn_cast<BitCastOperator>(ptr)) {
    ptr = cast->getOperand(0);
  }
  const auto *gep = dyn_cast<GEPOperator>(ptr);
  if (!gep || canonicalMemoryBase(gep->getPointerOperand()) != base ||
      gep->getNumIndices() == 0) {
    return false;
  }
  const auto *idx =
      dyn_cast<ConstantInt>(gep->getOperand(gep->getNumOperands() - 1));
  if (!idx) {
    return false;
  }
  index_out = idx->getZExtValue();
  return true;
}

} // namespace

MPIOpKind MPIProcessModel::classifyOperation(const Instruction *inst,
                                             ThreadAPI::TD_TYPE type) const {
  const MPISemanticDescriptor *descriptor = lookupMPISemantic(type);
  if (!descriptor) {
    return MPIOpKind::UNKNOWN;
  }

  if (descriptor->split_into_sendrecv) {
    return MPIOpKind::UNKNOWN;
  }

  if (descriptor->trait_driven_barrier_kind) {
    return thread_api_->isNonBlockingMPIBarrier(inst)
               ? MPIOpKind::BARRIER_NONBLOCKING
               : MPIOpKind::BARRIER_BLOCKING;
  }

  if (descriptor->trait_driven_collective_kind) {
    return thread_api_->isNonBlockingMPICollective(inst)
               ? MPIOpKind::COLLECTIVE_NONBLOCKING
               : MPIOpKind::COLLECTIVE_BLOCKING;
  }

  if (type == ThreadAPI::TD_MPI_COMM_CREATE && inst) {
    const Function *callee = thread_api_->getCallee(inst);
    if (callee && StringRef(thread_api_->getSemanticTag(callee))
                      .startswith("intercomm-")) {
      return MPIOpKind::INTERCOMM_CREATION;
    }
  }

  return descriptor->kind;
}

bool MPIProcessModel::tryGetConstantInt(const Value *value, int &out) const {
  const auto *ci = dyn_cast_or_null<ConstantInt>(value);
  if (!ci) {
    return false;
  }
  out = ci->getSExtValue();
  return true;
}

bool MPIProcessModel::tryGetScalarRange(const Value *value, int &min_out,
                                        int &max_out) const {
  if (!value) {
    return false;
  }
  if (const auto *ci = dyn_cast<ConstantInt>(value)) {
    min_out = ci->getSExtValue();
    max_out = min_out;
    return true;
  }
  if (rank_analysis_) {
    return rank_analysis_->tryEvaluateIntRange(value, min_out, max_out);
  }
  return false;
}

CommunicatorID
MPIProcessModel::canonicalizeCommunicator(const Value *communicator) const {
  if (!communicator) {
    return nullptr;
  }

  const Value *canonical = traceCommunicatorRoot(communicator);
  if (!canonical) {
    canonical = communicator->stripPointerCasts();
  }

  std::set<const Value *> visited;
  while (canonical && visited.insert(canonical).second) {
    auto it = canonical_communicators_.find(canonical);
    if (it == canonical_communicators_.end() || !it->second ||
        it->second == canonical) {
      break;
    }
    canonical = it->second->stripPointerCasts();
  }
  return canonical;
}

void MPIProcessModel::registerCommunicatorAlias(const Value *alias,
                                                const Value *root) {
  if (!alias) {
    return;
  }

  const Value *alias_key = alias->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(alias_key)) {
    alias_key = underlying->stripPointerCasts();
  }

  CommunicatorID canonical_root =
      root ? canonicalizeCommunicator(root) : alias_key;
  if (!canonical_root) {
    canonical_root = alias_key;
  }
  canonical_communicators_[alias_key] = canonical_root;
  assignCommunicatorClass(canonical_root);
}

void MPIProcessModel::registerCommunicatorSubgroup(const Value *alias,
                                                   const Value *root,
                                                   int subgroup_token) {
  if (!alias) {
    return;
  }
  registerCommunicatorAlias(alias, root);
  const Value *alias_key = alias->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(alias_key)) {
    alias_key = underlying->stripPointerCasts();
  }
  CommunicatorID canonical_root =
      root ? canonicalizeCommunicator(root) : alias_key;
  const std::string subgroup_key =
      std::to_string(assignCommunicatorClass(canonical_root)) + ":" +
      std::to_string(subgroup_token);
  size_t subgroup_id = std::hash<std::string>{}(subgroup_key) + 1;
  communicator_subgroup_ids_[alias_key] = subgroup_id;
  communicator_subgroup_ids_[canonical_root] = subgroup_id;
}

size_t MPIProcessModel::assignCommunicatorClass(CommunicatorID canonical) {
  if (!canonical) {
    return 0;
  }
  canonical = canonical->stripPointerCasts();

  if (const auto *arg = dyn_cast<Argument>(canonical)) {
    for (const auto &entry : communicator_class_ids_) {
      const auto *other_arg = dyn_cast<Argument>(entry.first);
      if (other_arg && other_arg->getArgNo() == arg->getArgNo()) {
        communicator_class_ids_[canonical] = entry.second;
        return entry.second;
      }
    }
  }

  auto it = communicator_class_ids_.find(canonical);
  if (it != communicator_class_ids_.end()) {
    return it->second;
  }
  size_t id = next_communicator_class_id_++;
  communicator_class_ids_[canonical] = id;
  return id;
}

size_t
MPIProcessModel::getCommunicatorClassID(CommunicatorID communicator) const {
  CommunicatorID canonical = canonicalizeCommunicator(communicator);
  if (!canonical) {
    return 0;
  }
  auto it = communicator_class_ids_.find(canonical);
  return it != communicator_class_ids_.end() ? it->second : 0;
}

size_t
MPIProcessModel::getCommunicatorSubgroupID(const Value *communicator) const {
  if (!communicator) {
    return 0;
  }
  const Value *key = traceCommunicatorRoot(communicator);
  if (!key) {
    key = communicator->stripPointerCasts();
  }
  auto it = communicator_subgroup_ids_.find(key);
  return it != communicator_subgroup_ids_.end() ? it->second : 0;
}

void MPIProcessModel::annotateRankConstraints(MPIOperation &op) const {
  if (!rank_analysis_ || !op.inst) {
    return;
  }

  MPI::RankExpr rank = rank_analysis_->getRankAtInstruction(op.inst);
  op.process_rank = rank;
  switch (rank_analysis_->getReachabilityAtInstruction(op.inst)) {
  case MPI::MPIRankAnalysis::ReachabilityKind::SomeRanks:
    op.protocol_reachability = ProtocolReachability::SomeRanks;
    if (rank.kind == MPI::RankExpr::Concrete) {
      op.rank_path_summary = ("rank==" + std::to_string(rank.concrete_value));
    } else {
      op.rank_path_summary = ("rank in [" + std::to_string(rank.range_min) +
                              ", " + std::to_string(rank.range_max) + "]");
    }
    break;
  case MPI::MPIRankAnalysis::ReachabilityKind::AllRanks:
    op.protocol_reachability = ProtocolReachability::AllRanks;
    op.rank_path_summary = "rank symbolic";
    break;
  case MPI::MPIRankAnalysis::ReachabilityKind::Unknown:
    op.protocol_reachability = ProtocolReachability::Unknown;
    op.rank_path_summary = "rank unknown";
    break;
  }
  auto assignRange = [](const MPI::RankExpr &expr, int concrete_value,
                        int &min_out, int &max_out) {
    if (concrete_value >= 0) {
      min_out = concrete_value;
      max_out = concrete_value;
      return;
    }
    if (expr.kind == MPI::RankExpr::Concrete) {
      min_out = expr.concrete_value;
      max_out = expr.concrete_value;
    } else if (expr.kind == MPI::RankExpr::Range) {
      min_out = expr.range_min;
      max_out = expr.range_max;
    }
  };
  assignRange(rank, op.source_rank, op.source_rank_min, op.source_rank_max);
  assignRange(rank, op.dest_rank, op.dest_rank_min, op.dest_rank_max);
  assignRange(rank, op.target_rank, op.target_rank_min, op.target_rank_max);
}

int64_t MPIProcessModel::getDatatypeExtent(const Value *datatype_arg,
                                           const Instruction *context) const {
  auto resolveBuiltinExtent = [](int datatype) {
    switch (datatype) {
    case 0:
      return int64_t(1);
    case 1:
      return int64_t(2);
    case 2:
      return int64_t(4);
    case 3:
      return int64_t(8);
    default:
      return int64_t(-1);
    }
  };

  const Value *canonical = canonicalizeDatatypeHandle(datatype_arg);
  if (canonical) {
    auto it = datatype_extent_bytes_.find(canonical);
    if (it != datatype_extent_bytes_.end()) {
      return it->second;
    }
  }

  if (const auto *load = dyn_cast_or_null<LoadInst>(datatype_arg)) {
    const Value *loaded_from =
        canonicalizeDatatypeHandle(load->getPointerOperand());
    if (loaded_from) {
      auto it = datatype_extent_bytes_.find(loaded_from);
      if (it != datatype_extent_bytes_.end()) {
        return it->second;
      }
    }
  }

  int datatype = 0;
  if (tryReadScalarInt(datatype_arg, datatype, context)) {
    return resolveBuiltinExtent(datatype);
  }
  return -1;
}

const Value *
MPIProcessModel::canonicalizeDatatypeHandle(const Value *handle) const {
  if (!handle) {
    return nullptr;
  }
  handle = handle->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(handle)) {
    handle = underlying->stripPointerCasts();
  }
  return handle;
}

void MPIProcessModel::registerDatatypeExtent(const Value *handle,
                                             int64_t extent) {
  if (!handle || extent <= 0) {
    return;
  }
  datatype_extent_bytes_[canonicalizeDatatypeHandle(handle)] = extent;
}

void MPIProcessModel::extractPointToPointDetails(
    MPIOperation &op, const CallBase *cb,
    const MPISemanticDescriptor &descriptor) {
  if (!cb) {
    return;
  }

  const Value *datatype = getOperandBySignedIndex(cb, descriptor.datatype_arg);
  op.datatype = datatype;
  op.datatype_size = getDatatypeExtent(op.datatype, op.inst);

  const Value *count_arg = getOperandBySignedIndex(cb, descriptor.count_arg);
  int count = 0;
  if (count_arg && tryReadScalarInt(count_arg, count, op.inst) &&
      op.datatype_size > 0) {
    op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
  }

  const Value *peer_arg = getOperandBySignedIndex(cb, descriptor.peer_rank_arg);
  if (peer_arg) {
    int value = -1;
    if (tryGetConstantInt(peer_arg, value)) {
      if (descriptor.peer_rank_is_dest) {
        op.dest_rank = value;
      } else {
        op.source_rank = value;
      }
    } else if (descriptor.peer_rank_is_dest) {
      tryGetScalarRange(peer_arg, op.dest_rank_min, op.dest_rank_max);
    } else {
      tryGetScalarRange(peer_arg, op.source_rank_min, op.source_rank_max);
    }
  }

  const Value *tag_arg = getOperandBySignedIndex(cb, descriptor.tag_arg);
  if (tag_arg) {
    int value = -1;
    if (tryGetConstantInt(tag_arg, value)) {
      op.tag = value;
    }
  }

  const Value *comm_arg =
      getOperandBySignedIndex(cb, descriptor.communicator_arg);
  if (comm_arg) {
    op.communicator = canonicalizeCommunicator(comm_arg);
  }

  const Value *request_arg =
      getOperandBySignedIndex(cb, descriptor.request_arg);
  if (request_arg) {
    op.request = request_arg;
  }
}

void MPIProcessModel::extractSendrecvDetails(MPIOperation &op,
                                             const CallBase *cb) const {
  if (!cb) {
    return;
  }

  unsigned num_args = cb->arg_size();
  int value = -1;
  if (num_args >= 11) {
    if (op.kind == MPIOpKind::SEND_BLOCKING) {
      op.datatype = cb->getArgOperand(2);
      op.datatype_size = getDatatypeExtent(op.datatype, op.inst);
      int count = 0;
      if (tryReadScalarInt(cb->getArgOperand(1), count, op.inst) &&
          op.datatype_size > 0) {
        op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
      }
      if (tryGetConstantInt(cb->getArgOperand(3), value)) {
        op.dest_rank = value;
      } else {
        tryGetScalarRange(cb->getArgOperand(3), op.dest_rank_min,
                          op.dest_rank_max);
      }
      if (tryGetConstantInt(cb->getArgOperand(4), value)) {
        op.tag = value;
      }
    } else {
      op.datatype = cb->getArgOperand(7);
      op.datatype_size = getDatatypeExtent(op.datatype, op.inst);
      int count = 0;
      if (tryReadScalarInt(cb->getArgOperand(6), count, op.inst) &&
          op.datatype_size > 0) {
        op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
      }
      if (tryGetConstantInt(cb->getArgOperand(8), value)) {
        op.source_rank = value;
      } else {
        tryGetScalarRange(cb->getArgOperand(8), op.source_rank_min,
                          op.source_rank_max);
      }
      if (tryGetConstantInt(cb->getArgOperand(9), value)) {
        op.tag = value;
      }
    }
    op.communicator = canonicalizeCommunicator(cb->getArgOperand(10));
    return;
  }

  if (num_args >= 8) {
    if (op.kind == MPIOpKind::SEND_BLOCKING) {
      op.datatype = cb->getArgOperand(2);
      op.datatype_size = getDatatypeExtent(op.datatype, op.inst);
      int count = 0;
      if (tryReadScalarInt(cb->getArgOperand(1), count, op.inst) &&
          op.datatype_size > 0) {
        op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
      }
      if (tryGetConstantInt(cb->getArgOperand(3), value)) {
        op.dest_rank = value;
      } else {
        tryGetScalarRange(cb->getArgOperand(3), op.dest_rank_min,
                          op.dest_rank_max);
      }
      if (tryGetConstantInt(cb->getArgOperand(4), value)) {
        op.tag = value;
      }
    } else {
      op.datatype = cb->getArgOperand(4);
      op.datatype_size = getDatatypeExtent(op.datatype, op.inst);
      int count = 0;
      if (tryReadScalarInt(cb->getArgOperand(3), count, op.inst) &&
          op.datatype_size > 0) {
        op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
      }
      if (tryGetConstantInt(cb->getArgOperand(5), value)) {
        op.source_rank = value;
      } else {
        tryGetScalarRange(cb->getArgOperand(5), op.source_rank_min,
                          op.source_rank_max);
      }
      if (tryGetConstantInt(cb->getArgOperand(6), value)) {
        op.tag = value;
      }
    }
    op.communicator = canonicalizeCommunicator(cb->getArgOperand(7));
  }
}

void MPIProcessModel::extractProbeDetails(
    MPIOperation &op, const CallBase *cb,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }
  const Value *peer_arg = getOperandBySignedIndex(cb, descriptor.peer_rank_arg);
  if (peer_arg) {
    int value = -1;
    if (tryGetConstantInt(peer_arg, value)) {
      op.source_rank = value;
    } else {
      tryGetScalarRange(peer_arg, op.source_rank_min, op.source_rank_max);
    }
  }

  const Value *tag_arg = getOperandBySignedIndex(cb, descriptor.tag_arg);
  if (tag_arg) {
    int value = -1;
    if (tryGetConstantInt(tag_arg, value)) {
      op.tag = value;
    }
  }

  const Value *comm_arg =
      getOperandBySignedIndex(cb, descriptor.communicator_arg);
  if (comm_arg) {
    op.communicator = canonicalizeCommunicator(comm_arg);
  }
}

void MPIProcessModel::extractCollectiveDetails(
    MPIOperation &op, const CallBase *cb, StringRef semantic_tag,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  const bool nonblocking = op.kind == MPIOpKind::BARRIER_NONBLOCKING ||
                           op.kind == MPIOpKind::COLLECTIVE_NONBLOCKING;
  int comm_index = descriptor.communicator_arg;
  if (nonblocking && descriptor.collective_nonblocking_comm_arg != -1) {
    comm_index = descriptor.collective_nonblocking_comm_arg;
  }
  const Value *comm_arg = getOperandBySignedIndex(cb, comm_index);
  if (comm_arg) {
    op.communicator = canonicalizeCommunicator(comm_arg);
    op.communicator_subgroup_id = getCommunicatorSubgroupID(comm_arg);
  }

  if (nonblocking) {
    const Value *request_arg = getOperandBySignedIndex(
        cb, descriptor.collective_nonblocking_request_arg);
    if (request_arg) {
      op.request = request_arg;
    }
  }

  if (semantic_tag.startswith("neighbor-") ||
      semantic_tag.startswith("ineighbor-")) {
    op.collective_protocol_class_id = 1;
  } else if (semantic_tag.startswith("intercomm-")) {
    op.collective_protocol_class_id = 2;
  } else {
    op.collective_protocol_class_id = 0;
  }
}

void MPIProcessModel::extractRequestDetails(
    MPIOperation &op, const CallBase *cb,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }
  const Value *request_arg =
      getOperandBySignedIndex(cb, descriptor.request_arg);
  if (request_arg) {
    op.request = request_arg;
    return;
  }

  if (op.td_type == ThreadAPI::TD_MPI_REQUEST_START) {
    op.request = getOperandBySignedIndex(cb, 0);
  }
}

void MPIProcessModel::extractRMAWindowDetails(
    MPIOperation &op, const CallBase *cb, StringRef semantic_tag,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }
  if (StringRef(semantic_tag).startswith("win-create") ||
      StringRef(semantic_tag).equals("win-allocate") ||
      StringRef(semantic_tag).equals("win-allocate-shared")) {
    if (cb->arg_size() >= 2) {
      const Value *comm_arg = getOperandBySignedIndex(cb, -2);
      const Value *window_arg = getOperandBySignedIndex(cb, -1);
      op.communicator = canonicalizeCommunicator(comm_arg);
      op.window = window_arg;
    }
    return;
  }

  if (StringRef(semantic_tag).equals("win-free")) {
    op.window = getOperandBySignedIndex(cb, descriptor.window_arg);
  }
}

void MPIProcessModel::extractRMADataDetails(
    MPIOperation &op, const CallBase *cb, StringRef semantic_tag,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }

  auto setByteLength = [&](int count_idx, int datatype_idx) {
    const Value *count_value = getOperandBySignedIndex(cb, count_idx);
    const Value *datatype_value = getOperandBySignedIndex(cb, datatype_idx);
    if (!count_value || !datatype_value) {
      return;
    }
    int count = 0;
    if (!tryReadScalarInt(count_value, count, op.inst)) {
      return;
    }
    int64_t extent = getDatatypeExtent(datatype_value, op.inst);
    if (extent <= 0) {
      return;
    }
    op.byte_length = static_cast<int64_t>(count) * extent;
  };

  int count_idx = descriptor.count_arg;
  int datatype_idx = descriptor.datatype_arg;
  int rank_idx = descriptor.target_rank_arg;
  int disp_idx = descriptor.target_disp_arg;
  int window_idx = descriptor.window_arg;

  if (StringRef(semantic_tag).equals("get-accumulate") ||
      StringRef(semantic_tag).equals("rget-accumulate")) {
    count_idx = 4;
    datatype_idx = 5;
    rank_idx = 6;
    disp_idx = 7;
    window_idx = 11;
  } else if (StringRef(semantic_tag).equals("fetch-and-op")) {
    count_idx = -1;
    datatype_idx = -1;
    rank_idx = 3;
    disp_idx = 4;
    window_idx = 6;
    op.byte_length = 1;
  } else if (StringRef(semantic_tag).equals("compare-and-swap")) {
    count_idx = -1;
    datatype_idx = -1;
    rank_idx = 4;
    disp_idx = 5;
    window_idx = 6;
    op.byte_length = 1;
  }

  if (count_idx >= 0 && datatype_idx >= 0) {
    setByteLength(count_idx, datatype_idx);
  }

  const Value *target_rank_arg = getOperandBySignedIndex(cb, rank_idx);
  if (target_rank_arg) {
    int value = -1;
    if (tryGetConstantInt(target_rank_arg, value)) {
      op.target_rank = value;
    } else {
      tryGetScalarRange(target_rank_arg, op.target_rank_min,
                        op.target_rank_max);
    }
  }

  const Value *disp_arg = getOperandBySignedIndex(cb, disp_idx);
  if (const auto *disp = dyn_cast_or_null<ConstantInt>(disp_arg)) {
    op.target_disp = disp->getSExtValue();
  }

  op.window = getOperandBySignedIndex(cb, window_idx);
}

void MPIProcessModel::extractRMASyncDetails(
    MPIOperation &op, const CallBase *cb, StringRef semantic_tag,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }

  StringRef tag = semantic_tag;
  if (tag.equals("win-fence")) {
    op.window = getOperandBySignedIndex(cb, 1);
    return;
  }

  if (tag.equals("win-lock")) {
    const Value *target_rank_arg = getOperandBySignedIndex(cb, 1);
    if (target_rank_arg) {
      int value = -1;
      if (tryGetConstantInt(target_rank_arg, value)) {
        op.target_rank = value;
      } else {
        tryGetScalarRange(target_rank_arg, op.target_rank_min,
                          op.target_rank_max);
      }
    }
    op.window = getOperandBySignedIndex(cb, 3);
    return;
  }

  if (tag.equals("win-lock-all")) {
    op.window = getOperandBySignedIndex(cb, 1);
    return;
  }

  if (tag.equals("win-unlock") || tag.equals("win-flush") ||
      tag.equals("win-flush-local")) {
    const Value *target_rank_arg = getOperandBySignedIndex(cb, 0);
    if (target_rank_arg) {
      int value = -1;
      if (tryGetConstantInt(target_rank_arg, value)) {
        op.target_rank = value;
      } else {
        tryGetScalarRange(target_rank_arg, op.target_rank_min,
                          op.target_rank_max);
      }
    }
    op.window = getOperandBySignedIndex(cb, 1);
    op.rma_local_completion_only = tag.equals("win-flush-local");
    return;
  }

  if (tag.equals("win-unlock-all") || tag.equals("win-flush-all") ||
      tag.equals("win-flush-local-all")) {
    op.window = getOperandBySignedIndex(cb, 0);
    op.rma_local_completion_only = tag.equals("win-flush-local-all");
    return;
  }

  if (tag.equals("win-sync") || tag.equals("win-complete") ||
      tag.equals("win-wait") || tag.equals("win-test")) {
    op.window = getOperandBySignedIndex(cb, 0);
    return;
  }

  if (tag.equals("win-post") || tag.equals("win-start")) {
    op.window = getOperandBySignedIndex(cb, 2);
    return;
  }

  if (descriptor.window_arg != -1) {
    op.window = getOperandBySignedIndex(cb, descriptor.window_arg);
  }
}

void MPIProcessModel::extractDatatypeDetails(MPIOperation &op,
                                             const CallBase *cb,
                                             StringRef semantic_tag) {
  if (!cb) {
    return;
  }

  auto readConstExtent = [&](const Value *value) -> int64_t {
    int int_value = 0;
    if (!tryReadScalarInt(value, int_value, op.inst)) {
      return -1;
    }
    return static_cast<int64_t>(int_value);
  };

  if (semantic_tag.equals("type-contiguous") && cb->arg_size() >= 3) {
    int64_t count = readConstExtent(cb->getArgOperand(0));
    int64_t base_extent = getDatatypeExtent(cb->getArgOperand(1), op.inst);
    if (count > 0 && base_extent > 0) {
      registerDatatypeExtent(cb->getArgOperand(2), count * base_extent);
      op.is_derived_datatype = true;
      op.datatype_size = count * base_extent;
      op.datatype = cb->getArgOperand(2);
    }
    return;
  }

  if ((semantic_tag.equals("type-vector") ||
       semantic_tag.equals("type-hvector") ||
       semantic_tag.equals("type-create-hvector")) &&
      cb->arg_size() >= 5) {
    int64_t count = readConstExtent(cb->getArgOperand(0));
    int64_t blocklength = readConstExtent(cb->getArgOperand(1));
    int64_t base_extent = getDatatypeExtent(cb->getArgOperand(3), op.inst);
    if (count > 0 && blocklength > 0 && base_extent > 0) {
      int64_t extent = count * blocklength * base_extent;
      registerDatatypeExtent(cb->getArgOperand(4), extent);
      op.is_derived_datatype = true;
      op.datatype_size = extent;
      op.datatype = cb->getArgOperand(4);
    }
    return;
  }

  if (semantic_tag.equals("type-create-resized") && cb->arg_size() >= 4) {
    int64_t extent = readConstExtent(cb->getArgOperand(2));
    if (extent > 0) {
      registerDatatypeExtent(cb->getArgOperand(3), extent);
      op.is_derived_datatype = true;
      op.datatype_size = extent;
      op.datatype = cb->getArgOperand(3);
    }
    return;
  }

  if (semantic_tag.equals("type-create-subarray") && cb->arg_size() >= 8) {
    int64_t base_extent = getDatatypeExtent(cb->getArgOperand(6), op.inst);
    if (base_extent > 0) {
      registerDatatypeExtent(cb->getArgOperand(7), base_extent);
      op.is_derived_datatype = true;
      op.datatype_size = base_extent;
      op.datatype = cb->getArgOperand(7);
    }
    return;
  }

  if (semantic_tag.equals("type-commit") && cb->arg_size() >= 1) {
    int64_t existing_extent = getDatatypeExtent(cb->getArgOperand(0), op.inst);
    if (existing_extent > 0) {
      registerDatatypeExtent(cb->getArgOperand(0), existing_extent);
    }
  }
}

void MPIProcessModel::extractOperationDetails(MPIOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb) {
    return;
  }
  const Function *callee = cb->getCalledFunction();
  const std::string semantic_tag_storage =
      callee ? thread_api_->getSemanticTag(callee) : std::string();
  const StringRef semantic_tag = semantic_tag_storage;

  const MPISemanticDescriptor *descriptor = lookupMPISemantic(op.td_type);
  if (!descriptor) {
    return;
  }

  switch (descriptor->family) {
  case MPISemanticFamily::PointToPoint:
    if (descriptor->split_into_sendrecv) {
      extractSendrecvDetails(op, cb);
    } else {
      extractPointToPointDetails(op, cb, *descriptor);
    }
    break;
  case MPISemanticFamily::Probe:
    extractProbeDetails(op, cb, *descriptor);
    break;
  case MPISemanticFamily::Request:
    if (op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT ||
        op.td_type == ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT) {
      extractPointToPointDetails(op, cb, *descriptor);
    }
    extractRequestDetails(op, cb, *descriptor);
    break;
  case MPISemanticFamily::Collective:
    extractCollectiveDetails(op, cb, semantic_tag, *descriptor);
    break;
  case MPISemanticFamily::RMAWindow:
    extractRMAWindowDetails(op, cb, semantic_tag, *descriptor);
    break;
  case MPISemanticFamily::RMAData:
    extractRMADataDetails(op, cb, semantic_tag, *descriptor);
    break;
  case MPISemanticFamily::RMASync:
    extractRMASyncDetails(op, cb, semantic_tag, *descriptor);
    break;
  case MPISemanticFamily::Datatype:
    extractDatatypeDetails(op, cb, semantic_tag);
    break;
  default:
    break;
  }
}

void MPIProcessModel::analyzeModule() {
  all_operations_.clear();
  non_blocking_ops_.clear();
  persistent_request_templates_.clear();
  operation_kind_counts_.clear();
  canonical_communicators_.clear();
  communicator_class_ids_.clear();
  next_communicator_class_id_ = 1;
  communicator_subgroup_ids_.clear();
  next_communicator_subgroup_id_ = 1;
  deferred_lowering_stats_.clear();
  datatype_extent_bytes_.clear();
  normalization_confidence_counts_.clear();
  init_thread_required_level_ = -1;
  has_init_thread_level_ = false;
  init_thread_provided_level_ = -1;
  has_provided_init_thread_level_ = false;
  rank_analysis_ = std::make_unique<MPI::MPIRankAnalysis>(module_);
  rank_analysis_->analyze();

  for (Function &F : module_) {
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      Instruction *I = &*II;

      const Function *callee = thread_api_->getCallee(I);
      if (!callee)
        continue;

      MPIEffect effect = buildMPIEffect(I, thread_api_);
      ThreadAPI::TD_TYPE type = effect.type;
      if (type == ThreadAPI::TD_DUMMY)
        continue;
      normalization_confidence_counts_[effect.confidence]++;

      const MPISemanticDescriptor *descriptor = effect.descriptor;
      if (descriptor && descriptor->split_into_sendrecv) {
        MPIOperation send_op(I, MPIOpKind::SEND_BLOCKING, type);
        send_op.normalization_confidence = effect.confidence;
        extractOperationDetails(send_op);
        annotateRankConstraints(send_op);
        if (send_op.communicator) {
          send_op.communicator_class_id = assignCommunicatorClass(
              canonicalizeCommunicator(send_op.communicator));
        }
        all_operations_.push_back(send_op);
        ++operation_kind_counts_[send_op.kind];

        MPIOperation recv_op(I, MPIOpKind::RECV_BLOCKING, type);
        recv_op.normalization_confidence = effect.confidence;
        extractOperationDetails(recv_op);
        annotateRankConstraints(recv_op);
        if (recv_op.communicator) {
          recv_op.communicator_class_id = assignCommunicatorClass(
              canonicalizeCommunicator(recv_op.communicator));
        }
        all_operations_.push_back(recv_op);
        ++operation_kind_counts_[recv_op.kind];
        continue;
      }

      MPIOpKind kind = classifyOperation(I, type);
      if (kind == MPIOpKind::UNKNOWN)
        continue;

      MPIOperation op(I, kind, type);
      op.normalization_confidence = effect.confidence;
      extractOperationDetails(op);

      if (kind == MPIOpKind::INIT && callee) {
        std::string semantic_tag_storage = thread_api_->getSemanticTag(callee);
        StringRef semantic_tag = semantic_tag_storage;
        if (semantic_tag.equals("init-thread")) {
          const auto *cb = dyn_cast<CallBase>(I);
          int required_level = -1;
          if (cb && tryReadScalarInt(getOperandBySignedIndex(cb, 2),
                                     required_level, I)) {
            has_init_thread_level_ = true;
            init_thread_required_level_ = required_level;
          }
          int provided_level = -1;
          if (cb && tryReadScalarInt(getOperandBySignedIndex(cb, 3),
                                     provided_level, I)) {
            has_provided_init_thread_level_ = true;
            init_thread_provided_level_ = provided_level;
          }
        }
      }

      annotateRankConstraints(op);
      if (op.communicator) {
        op.communicator_class_id =
            assignCommunicatorClass(canonicalizeCommunicator(op.communicator));
        if (op.communicator_subgroup_id == 0 &&
            op.protocol_reachability == ProtocolReachability::SomeRanks) {
          op.communicator_subgroup_id =
              std::hash<std::string>{}(op.rank_path_summary) + 1;
        }
      }

      if (kind == MPIOpKind::COMM_MANAGEMENT ||
          kind == MPIOpKind::INTERCOMM_CREATION) {
        if (const auto *cb = dyn_cast<CallBase>(I)) {
          const Value *root =
              cb->arg_size() >= 1 ? cb->getArgOperand(0) : nullptr;

          if (kind == MPIOpKind::INTERCOMM_CREATION && cb->arg_size() >= 1) {
            registerCommunicatorAlias(cb->getArgOperand(cb->arg_size() - 1),
                                      root);
          } else if (type == ThreadAPI::TD_MPI_COMM_DUP &&
                     cb->arg_size() >= 2) {
            registerCommunicatorAlias(cb->getArgOperand(1), root);
          } else if (type == ThreadAPI::TD_MPI_COMM_SPLIT &&
                     cb->arg_size() >= 4) {
            int color = 0;
            if (tryReadScalarInt(cb->getArgOperand(1), color, I)) {
              registerCommunicatorSubgroup(
                  cb->getArgOperand(cb->arg_size() - 1), root, color);
            } else {
              registerCommunicatorAlias(cb->getArgOperand(cb->arg_size() - 1),
                                        root);
            }
          } else if (type == ThreadAPI::TD_MPI_COMM_CREATE &&
                     cb->arg_size() >= 3) {
            registerCommunicatorAlias(cb->getArgOperand(cb->arg_size() - 1),
                                      root);
          }
        }
      }

      all_operations_.push_back(op);
      ++operation_kind_counts_[kind];

      if (kind == MPIOpKind::REQUEST_MANAGEMENT && op.request) {
        if (op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT ||
            op.td_type == ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT) {
          NonBlockingOp persistent_op;
          persistent_op.issue_inst = I;
          persistent_op.request = op.request;
          persistent_op.completion_state = RequestCompletionState::Created;
          persistent_op.peer_rank =
              op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT
                  ? op.dest_rank
                  : op.source_rank;
          persistent_op.tag = op.tag;
          persistent_op.comm = op.communicator;
          persistent_request_templates_[op.request] = persistent_op;
        }
      }

      if (kind == MPIOpKind::SEND_NONBLOCKING ||
          kind == MPIOpKind::RECV_NONBLOCKING ||
          kind == MPIOpKind::BARRIER_NONBLOCKING ||
          kind == MPIOpKind::COLLECTIVE_NONBLOCKING) {
        if (op.request) {
          NonBlockingOp nbOp;
          nbOp.issue_inst = I;
          nbOp.request = op.request;
          nbOp.completion_state = RequestCompletionState::Active;
          nbOp.peer_rank = (kind == MPIOpKind::SEND_NONBLOCKING)
                               ? op.dest_rank
                               : op.source_rank;
          nbOp.tag = op.tag;
          nbOp.comm = op.communicator;
          non_blocking_ops_[op.request] = nbOp;
        }
      }
    }
  }

  std::map<std::tuple<size_t, size_t, const Function *>, size_t>
      protocol_sequence_by_scope;
  for (MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::BARRIER_BLOCKING &&
        op.kind != MPIOpKind::BARRIER_NONBLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }
    auto key = std::make_tuple(op.communicator_class_id,
                               op.communicator_subgroup_id, op.function);
    op.protocol_sequence_id = protocol_sequence_by_scope[key]++;
    op.semantic_relation.kind = concurrency::RelationKind::SameProtocolSlot;
    op.semantic_relation.proof =
        op.protocol_reachability == ProtocolReachability::AllRanks
            ? concurrency::ProofStrength::Must
            : concurrency::ProofStrength::May;
    if (op.semantic_relation.proof == concurrency::ProofStrength::Must &&
        has_provided_init_thread_level_ &&
        disablesDeterministicMPIOrdering(init_thread_provided_level_)) {
      op.semantic_relation.proof = concurrency::ProofStrength::May;
      op.semantic_relation.reason =
          "mpi_collective_protocol_slot_thread_downgrade";
      continue;
    }
    op.semantic_relation.reason = "mpi_collective_protocol_slot";
  }

  matchNonBlockingOps();

  if (!deferred_lowering_stats_.empty()) {
    errs() << "MPI deferred lowering:";
    for (const auto &entry : deferred_lowering_stats_) {
      errs() << " " << entry.first << "=" << entry.second;
    }
    errs() << "\n";
  }
}

std::vector<RequestID>
MPIProcessModel::collectRequestOperands(const Value *request_arg,
                                        const Instruction *context) const {
  std::vector<RequestID> requests;
  if (!request_arg) {
    return requests;
  }

  const Value *base = canonicalMemoryBase(request_arg);

  if (const auto *gv = dyn_cast<GlobalVariable>(base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *array = dyn_cast<ConstantArray>(init)) {
        for (unsigned i = 0; i < array->getNumOperands(); ++i) {
          if (const auto *elem = dyn_cast<Constant>(array->getOperand(i))) {
            requests.push_back(elem->stripPointerCasts());
          }
        }
      }
    }
  } else if (const auto *alloca = dyn_cast<AllocaInst>(base)) {
    if (context && context->getFunction() == alloca->getFunction()) {
      std::map<uint64_t, RequestID> same_block_requests;
      for (const Instruction &inst : *context->getParent()) {
        if (&inst == context) {
          break;
        }
        const auto *store = dyn_cast<StoreInst>(&inst);
        uint64_t index = 0;
        if (!store || !getIndexedStoreTarget(store, base, index)) {
          continue;
        }
        same_block_requests[index] =
            store->getValueOperand()->stripPointerCasts();
      }
      if (!same_block_requests.empty()) {
        for (const auto &entry : same_block_requests) {
          requests.push_back(entry.second);
        }
        return requests;
      }
    }

    std::map<uint64_t, std::set<RequestID>> indexed_requests;
    bool ambiguous_index = false;
    for (const Instruction &inst : instructions(alloca->getFunction())) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store || (context && !mayDefinitionReach(store, context))) {
        continue;
      }
      uint64_t index = 0;
      if (!getIndexedStoreTarget(store, base, index)) {
        continue;
      }
      const Value *stored = store->getValueOperand()->stripPointerCasts();
      if (!stored) {
        ambiguous_index = true;
        continue;
      }
      indexed_requests[index].insert(stored);
    }
    if (ambiguous_index) {
      return {};
    }
    for (const auto &entry : indexed_requests) {
      if (entry.second.size() != 1) {
        return {};
      }
      requests.push_back(*entry.second.begin());
    }
  }

  if (requests.empty()) {
    requests.push_back(request_arg->stripPointerCasts());
  }
  return requests;
}

std::vector<int> MPIProcessModel::collectCompletedRequestIndices(
    const Value *indices_arg, size_t bound, const Instruction *context) const {
  std::set<int> completed;
  if (!indices_arg || bound == 0) {
    return {};
  }

  const Value *base = canonicalMemoryBase(indices_arg);

  if (const auto *gv = dyn_cast<GlobalVariable>(base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *array = dyn_cast<ConstantArray>(init)) {
        for (unsigned i = 0; i < array->getNumOperands(); ++i) {
          const auto *ci = dyn_cast<ConstantInt>(array->getOperand(i));
          if (!ci) {
            continue;
          }
          int index = ci->getSExtValue();
          if (index >= 0 && static_cast<size_t>(index) < bound) {
            completed.insert(index);
          }
        }
      }
    }
  } else if (const auto *alloca = dyn_cast<AllocaInst>(base)) {
    if (context && context->getFunction() == alloca->getFunction()) {
      std::map<uint64_t, int> same_block_values;
      for (const Instruction &inst : *context->getParent()) {
        if (&inst == context) {
          break;
        }
        const auto *store = dyn_cast<StoreInst>(&inst);
        const auto *stored_idx =
            store ? dyn_cast<ConstantInt>(store->getValueOperand()) : nullptr;
        uint64_t array_index = 0;
        if (!stored_idx || !getIndexedStoreTarget(store, base, array_index)) {
          continue;
        }
        int index = stored_idx->getSExtValue();
        if (index >= 0 && static_cast<size_t>(index) < bound) {
          same_block_values[array_index] = index;
        }
      }
      if (!same_block_values.empty()) {
        for (const auto &entry : same_block_values) {
          completed.insert(entry.second);
        }
        std::vector<int> result;
        result.reserve(completed.size());
        for (int index : completed) {
          result.push_back(index);
        }
        return result;
      }
    }

    std::map<uint64_t, std::set<int>> values_by_slot;
    for (const Instruction &inst : instructions(alloca->getFunction())) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store || (context && !mayDefinitionReach(store, context))) {
        continue;
      }
      const auto *stored_idx = dyn_cast<ConstantInt>(store->getValueOperand());
      uint64_t array_index = 0;
      if (!stored_idx || !getIndexedStoreTarget(store, base, array_index)) {
        continue;
      }
      int index = stored_idx->getSExtValue();
      if (index >= 0 && static_cast<size_t>(index) < bound) {
        values_by_slot[array_index].insert(index);
      }
    }
    for (const auto &entry : values_by_slot) {
      if (entry.second.size() != 1) {
        return {};
      }
      completed.insert(*entry.second.begin());
    }
  }

  std::vector<int> result;
  result.reserve(completed.size());
  for (int index : completed) {
    result.push_back(index);
  }
  return result;
}

bool MPIProcessModel::tryReadScalarInt(const Value *scalar_arg, int &out,
                                       const Instruction *context) const {
  if (!scalar_arg) {
    return false;
  }

  if (const auto *ci = dyn_cast<ConstantInt>(scalar_arg)) {
    out = ci->getSExtValue();
    return true;
  }

  const Value *base = canonicalMemoryBase(scalar_arg);

  std::set<int> seen_values;
  if (const auto *gv = dyn_cast<GlobalVariable>(base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *ci = dyn_cast<ConstantInt>(init)) {
        seen_values.insert(ci->getSExtValue());
      }
    }
  } else if (const auto *alloca = dyn_cast<AllocaInst>(base)) {
    if (context && context->getFunction() == alloca->getFunction()) {
      const StoreInst *same_block_store = nullptr;
      for (const Instruction &inst : *context->getParent()) {
        if (&inst == context) {
          break;
        }
        const auto *store = dyn_cast<StoreInst>(&inst);
        if (store && isDirectStoreToLocation(store, base)) {
          same_block_store = store;
        }
      }
      if (same_block_store) {
        if (const auto *stored =
                dyn_cast<ConstantInt>(same_block_store->getValueOperand())) {
          out = stored->getSExtValue();
          return true;
        }
        return false;
      }
    }

    const Function *parent = alloca->getFunction();
    if (!parent) {
      return false;
    }
    for (const Instruction &inst : instructions(parent)) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store || (context && !mayDefinitionReach(store, context))) {
        continue;
      }
      if (!isDirectStoreToLocation(store, base)) {
        continue;
      }
      if (const auto *stored =
              dyn_cast<ConstantInt>(store->getValueOperand())) {
        seen_values.insert(stored->getSExtValue());
      } else {
        return false;
      }
    }
  }

  if (seen_values.size() != 1) {
    return false;
  }
  out = *seen_values.begin();
  return true;
}

void MPIProcessModel::matchNonBlockingOps() {
  for (const MPIOperation &op : all_operations_) {
    if (op.kind == MPIOpKind::REQUEST_MANAGEMENT &&
        op.td_type == ThreadAPI::TD_MPI_REQUEST_START && op.request) {
      std::vector<RequestID> requests =
          collectRequestOperands(op.request, op.inst);
      for (RequestID request : requests) {
        auto it = persistent_request_templates_.find(request);
        if (it == persistent_request_templates_.end()) {
          continue;
        }
        NonBlockingOp active_op = it->second;
        active_op.issue_inst = op.inst;
        active_op.completion_state = RequestCompletionState::Active;
        active_op.wait_inst = nullptr;
        non_blocking_ops_[request] = active_op;
      }
      continue;
    }
    if (op.kind == MPIOpKind::REQUEST_MANAGEMENT && op.request) {
      std::vector<RequestID> requests =
          collectRequestOperands(op.request, op.inst);
      for (RequestID request : requests) {
        auto it = non_blocking_ops_.find(request);
        if (it == non_blocking_ops_.end()) {
          continue;
        }
        it->second.completion_state = RequestCompletionState::Freed;
        it->second.wait_inst = op.inst;
      }
      continue;
    }
    if (op.kind != MPIOpKind::WAIT && op.kind != MPIOpKind::TEST)
      continue;
    if (!op.request)
      continue;

    std::vector<RequestID> requests =
        collectRequestOperands(op.request, op.inst);
    if (requests.empty()) {
      continue;
    }

    auto markObserved = [&](RequestID request) {
      auto it = non_blocking_ops_.find(request);
      if (it != non_blocking_ops_.end()) {
        it->second.wait_inst = op.inst;
      }
    };
    auto markCompleted = [&](RequestID request, MPIRequestState state) {
      auto it = non_blocking_ops_.find(request);
      if (it != non_blocking_ops_.end()) {
        it->second.wait_inst = op.inst;
        it->second.completion_state = state;
      }
    };
    auto markAllMayComplete =
        [&](const std::vector<RequestID> &pending_requests) {
          for (RequestID request : pending_requests) {
            auto it = non_blocking_ops_.find(request);
            if (it == non_blocking_ops_.end()) {
              continue;
            }
            if (it->second.completion_state == MPIRequestState::Pending ||
                it->second.completion_state == MPIRequestState::Active ||
                it->second.completion_state == MPIRequestState::Created) {
              it->second.completion_state = MPIRequestState::MayComplete;
            }
            it->second.wait_inst = op.inst;
          }
        };

    for (RequestID request : requests) {
      markObserved(request);
    }

    const CallBase *cb = dyn_cast<CallBase>(op.inst);
    switch (op.td_type) {
    case ThreadAPI::TD_MPI_WAIT:
      markCompleted(requests.front(), MPIRequestState::MustComplete);
      break;
    case ThreadAPI::TD_MPI_TEST: {
      bool test_true = false;
      bool flag_unknown = false;
      if (cb && cb->arg_size() >= 2) {
        int flag = 0;
        if (tryReadScalarInt(cb->getArgOperand(1), flag, op.inst)) {
          test_true = flag != 0;
        } else {
          deferred_lowering_stats_["unknown_flag_value"]++;
          deferred_lowering_stats_["test_unknown_flag"]++;
          flag_unknown = true;
        }
      } else {
        deferred_lowering_stats_["unknown_flag_value"]++;
        deferred_lowering_stats_["test_unknown_flag"]++;
        flag_unknown = true;
      }
      if (test_true) {
        markCompleted(requests.front(), MPIRequestState::MustComplete);
      } else if (flag_unknown) {
        markCompleted(requests.front(), MPIRequestState::MayComplete);
      }
      break;
    }
    case ThreadAPI::TD_MPI_WAITALL:
      for (RequestID request : requests) {
        markCompleted(request, RequestCompletionState::MustComplete);
      }
      break;
    case ThreadAPI::TD_MPI_TESTALL: {
      bool testall_true = false;
      bool flag_unknown = false;
      if (cb && cb->arg_size() >= 3) {
        int flag = 0;
        if (tryReadScalarInt(cb->getArgOperand(2), flag, op.inst)) {
          testall_true = flag != 0;
        } else {
          deferred_lowering_stats_["testall_unknown_flag"]++;
          flag_unknown = true;
        }
      } else {
        deferred_lowering_stats_["testall_unknown_flag"]++;
        flag_unknown = true;
      }
      if (testall_true) {
        for (RequestID request : requests) {
          markCompleted(request, MPIRequestState::MustComplete);
        }
      } else if (flag_unknown) {
        markAllMayComplete(requests);
      }
      break;
    }
    case ThreadAPI::TD_MPI_WAITANY: {
      int selected = -1;
      if (cb && cb->arg_size() >= 3) {
        if (!tryReadScalarInt(cb->getArgOperand(2), selected, op.inst)) {
          deferred_lowering_stats_["waitany_unknown_index"]++;
        }
      } else {
        deferred_lowering_stats_["waitany_unknown_index"]++;
      }
      if (selected >= 0 && static_cast<size_t>(selected) < requests.size()) {
        markCompleted(requests[static_cast<size_t>(selected)],
                      MPIRequestState::MustComplete);
      } else {
        markAllMayComplete(requests);
      }
      break;
    }
    case ThreadAPI::TD_MPI_TESTANY: {
      bool testany_true = false;
      int selected = -1;
      bool selected_known = false;
      if (cb && cb->arg_size() >= 4) {
        int flag = 0;
        if (tryReadScalarInt(cb->getArgOperand(3), flag, op.inst)) {
          testany_true = flag != 0;
        } else {
          deferred_lowering_stats_["unknown_flag_value"]++;
          deferred_lowering_stats_["testany_unknown_flag"]++;
        }
        if (tryReadScalarInt(cb->getArgOperand(2), selected, op.inst)) {
          selected_known = true;
        } else {
          deferred_lowering_stats_["unknown_completed_index_set"]++;
          deferred_lowering_stats_["testany_unknown_index"]++;
        }
      } else {
        deferred_lowering_stats_["unknown_flag_value"]++;
        deferred_lowering_stats_["testany_unknown_flag"]++;
      }
      if (testany_true && selected_known && selected >= 0 &&
          static_cast<size_t>(selected) < requests.size()) {
        markCompleted(requests[static_cast<size_t>(selected)],
                      MPIRequestState::MustComplete);
      } else if (testany_true) {
        markAllMayComplete(requests);
      }
      break;
    }
    case ThreadAPI::TD_MPI_WAITSOME: {
      std::vector<int> completed_indices;
      if (cb && cb->arg_size() >= 4) {
        completed_indices = collectCompletedRequestIndices(
            cb->getArgOperand(3), requests.size(), op.inst);
      }
      if (completed_indices.empty()) {
        deferred_lowering_stats_["waitsome_unknown_indices"]++;
        markAllMayComplete(requests);
        break;
      }
      for (int index : completed_indices) {
        if (index < 0 || static_cast<size_t>(index) >= requests.size()) {
          continue;
        }
        markCompleted(requests[static_cast<size_t>(index)],
                      MPIRequestState::MustComplete);
      }
      break;
    }
    case ThreadAPI::TD_MPI_TESTSOME: {
      int outcount = 0;
      bool has_positive_outcount = false;
      bool outcount_unknown = false;
      if (cb && cb->arg_size() >= 3) {
        if (tryReadScalarInt(cb->getArgOperand(2), outcount, op.inst)) {
          has_positive_outcount = outcount > 0;
        } else {
          deferred_lowering_stats_["testsome_unknown_outcount"]++;
          outcount_unknown = true;
        }
      } else {
        deferred_lowering_stats_["testsome_unknown_outcount"]++;
        outcount_unknown = true;
      }
      if (!has_positive_outcount) {
        if (outcount_unknown) {
          markAllMayComplete(requests);
        }
        break;
      }

      std::vector<int> completed_indices;
      if (cb && cb->arg_size() >= 4) {
        completed_indices = collectCompletedRequestIndices(
            cb->getArgOperand(3), requests.size(), op.inst);
      }
      if (completed_indices.empty()) {
        deferred_lowering_stats_["testsome_unknown_indices"]++;
        markAllMayComplete(requests);
        break;
      }
      for (int index : completed_indices) {
        if (index < 0 || static_cast<size_t>(index) >= requests.size()) {
          continue;
        }
        markCompleted(requests[static_cast<size_t>(index)],
                      MPIRequestState::MustComplete);
      }
      break;
    }
    default:
      break;
    }
  }

  for (MPIOperation &issued_op : all_operations_) {
    if (!issued_op.request) {
      continue;
    }
    auto it = non_blocking_ops_.find(issued_op.request);
    if (it == non_blocking_ops_.end()) {
      continue;
    }
    issued_op.request_state = it->second.completion_state;
    issued_op.completion_inst = it->second.wait_inst;
  }
}

std::vector<MPIOperation>
MPIProcessModel::getOperationsByKind(MPIOpKind kind) const {
  std::vector<MPIOperation> result;
  for (const MPIOperation &op : all_operations_) {
    if (op.kind == kind) {
      result.push_back(op);
    }
  }
  return result;
}

MPICommunicationMatch
MPIProcessModel::classifyCommunicationMatch(const MPIOperation &op1,
                                            const MPIOperation &op2) const {
  bool op1_is_send = (op1.kind == MPIOpKind::SEND_BLOCKING ||
                      op1.kind == MPIOpKind::SEND_NONBLOCKING);
  bool op2_is_send = (op2.kind == MPIOpKind::SEND_BLOCKING ||
                      op2.kind == MPIOpKind::SEND_NONBLOCKING);

  if (op1_is_send == op2_is_send) {
    return MPICommunicationMatch::NoMatch;
  }

  const MPIOperation &send = op1_is_send ? op1 : op2;
  const MPIOperation &recv = op1_is_send ? op2 : op1;
  bool precise = true;

  if (!isMPIWildcardValue(send.dest_rank) &&
      !isMPIWildcardValue(recv.source_rank) &&
      send.dest_rank != recv.source_rank) {
    if (!rangesOverlap(send.dest_rank_min, send.dest_rank_max,
                       recv.source_rank_min, recv.source_rank_max)) {
      return MPICommunicationMatch::NoMatch;
    }
  } else if (!rangesOverlap(send.dest_rank_min, send.dest_rank_max,
                            recv.source_rank_min, recv.source_rank_max)) {
    return MPICommunicationMatch::NoMatch;
  } else if (isMPIWildcardValue(send.dest_rank) ||
             isMPIWildcardValue(recv.source_rank) || send.dest_rank < 0 ||
             recv.source_rank < 0) {
    precise = false;
  }

  if (!isMPIWildcardValue(send.tag) && !isMPIWildcardValue(recv.tag) &&
      send.tag != recv.tag) {
    return MPICommunicationMatch::NoMatch;
  }
  if (isMPIWildcardValue(send.tag) || isMPIWildcardValue(recv.tag) ||
      send.tag < 0 || recv.tag < 0) {
    precise = false;
  }

  if (send.communicator_class_id != 0 && recv.communicator_class_id != 0 &&
      send.communicator_class_id == recv.communicator_class_id) {
  } else if (send.communicator && recv.communicator &&
             !communicatorsMayAlias(send.communicator, recv.communicator)) {
    return MPICommunicationMatch::NoMatch;
  } else {
    precise = false;
  }

  if (send.protocol_reachability == ProtocolReachability::SomeRanks &&
      recv.protocol_reachability == ProtocolReachability::SomeRanks &&
      send.rank_path_summary != recv.rank_path_summary) {
    precise = false;
  }

  if (send.datatype_size > 0 && recv.datatype_size > 0 &&
      send.datatype_size != recv.datatype_size) {
    return MPICommunicationMatch::NoMatch;
  }

  return precise ? MPICommunicationMatch::MustMatch
                 : MPICommunicationMatch::MayMatch;
}

bool MPIProcessModel::canCommunicate(const MPIOperation &op1,
                                     const MPIOperation &op2) const {
  MPICommunicationMatch match = classifyCommunicationMatch(op1, op2);
  return match == MPICommunicationMatch::MustMatch ||
         match == MPICommunicationMatch::MayMatch;
}

std::vector<MPIProcessModel::NonBlockingOp>
MPIProcessModel::findOrphanedNonBlockingOps() const {
  std::vector<NonBlockingOp> orphaned;
  for (const auto &pair : non_blocking_ops_) {
    if (pair.second.completion_state == MPIRequestState::Pending ||
        pair.second.completion_state == MPIRequestState::Active ||
        pair.second.completion_state == MPIRequestState::Created) {
      orphaned.push_back(pair.second);
    }
  }
  return orphaned;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findPotentialDeadlocks() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> deadlocks;

  std::map<const Function *, std::vector<const MPIOperation *>> ops_by_func;
  for (const MPIOperation &op : all_operations_) {
    if (!op.function) {
      continue;
    }
    ops_by_func[op.function].push_back(&op);
  }

  struct BlockingWaitState {
    const MPIOperation *send = nullptr;
    const MPIOperation *recv = nullptr;
    const Function *function = nullptr;
  };

  std::vector<BlockingWaitState> wait_states;
  for (const auto &entry : ops_by_func) {
    const auto &ops = entry.second;
    for (size_t i = 0; i < ops.size(); ++i) {
      const MPIOperation *send = ops[i];
      if (!send || send->kind != MPIOpKind::SEND_BLOCKING) {
        continue;
      }
      for (size_t j = i + 1; j < ops.size(); ++j) {
        const MPIOperation *recv = ops[j];
        if (!recv || recv->kind != MPIOpKind::RECV_BLOCKING) {
          continue;
        }
        if (!sameCommunicatorForProof(*send, *recv)) {
          continue;
        }
        wait_states.push_back({send, recv, entry.first});
        break;
      }
    }
  }

  auto opMayExecuteOnRank = [](const MPIOperation &op, int rank) {
    if (rank < 0) {
      return true;
    }
    if (op.process_rank.kind == MPI::RankExpr::Concrete) {
      return op.process_rank.concrete_value == rank;
    }
    if (op.process_rank.kind == MPI::RankExpr::Range) {
      return rank >= op.process_rank.range_min &&
             rank <= op.process_rank.range_max;
    }
    return true;
  };

  auto sendMayBlockOnState = [&](const MPIOperation &send,
                                 const BlockingWaitState &state) {
    if (!state.send || !state.recv) {
      return false;
    }
    if (!sameCommunicatorForProof(send, *state.recv)) {
      return false;
    }
    if (!isMPIWildcardValue(send.dest_rank) &&
        !opMayExecuteOnRank(*state.recv, send.dest_rank)) {
      return false;
    }
    if (!isMPIWildcardValue(state.recv->source_rank) &&
        !opMayExecuteOnRank(send, state.recv->source_rank)) {
      return false;
    }
    if (!isMPIWildcardValue(send.tag) && !isMPIWildcardValue(state.recv->tag) &&
        send.tag != state.recv->tag) {
      return false;
    }
    return true;
  };

  std::vector<std::vector<size_t>> graph(wait_states.size());
  for (size_t i = 0; i < wait_states.size(); ++i) {
    for (size_t j = 0; j < wait_states.size(); ++j) {
      if (i == j || wait_states[i].function == wait_states[j].function ||
          !wait_states[i].send || !wait_states[j].recv) {
        continue;
      }
      if (sendMayBlockOnState(*wait_states[i].send, wait_states[j])) {
        graph[i].push_back(j);
      }
    }
  }

  std::set<std::pair<const Instruction *, const Instruction *>>
      unique_deadlocks;

  std::vector<size_t> stack;
  std::vector<int> color(wait_states.size(), 0);
  std::function<void(size_t)> dfs = [&](size_t node) {
    color[node] = 1;
    stack.push_back(node);
    for (size_t succ : graph[node]) {
      if (color[succ] == 0) {
        dfs(succ);
        continue;
      }
      if (color[succ] != 1) {
        continue;
      }
      auto begin = std::find(stack.begin(), stack.end(), succ);
      if (begin == stack.end()) {
        continue;
      }
      std::vector<size_t> cycle(begin, stack.end());
      if (cycle.size() < 2) {
        continue;
      }
      if (cycle.size() == 2) {
        const BlockingWaitState &lhs = wait_states[cycle[0]];
        const BlockingWaitState &rhs = wait_states[cycle[1]];
        if (lhs.send && rhs.send) {
          const Instruction *first = lhs.send->inst;
          const Instruction *second = rhs.send->inst;
          if (second < first) {
            std::swap(first, second);
          }
          unique_deadlocks.emplace(first, second);
        }
        continue;
      }
      for (size_t idx = 0; idx < cycle.size(); ++idx) {
        const BlockingWaitState &lhs = wait_states[cycle[idx]];
        const BlockingWaitState &rhs =
            wait_states[cycle[(idx + 1) % cycle.size()]];
        if (lhs.send && rhs.send) {
          unique_deadlocks.emplace(lhs.send->inst, rhs.send->inst);
        }
      }
    }
    stack.pop_back();
    color[node] = 2;
  };
  for (size_t i = 0; i < wait_states.size(); ++i) {
    if (color[i] == 0) {
      dfs(i);
    }
  }

  for (const auto &pair : unique_deadlocks) {
    deadlocks.push_back(pair);
  }

  return deadlocks;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findTagMismatches() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> mismatches;

  std::map<std::pair<size_t, size_t>,
           std::pair<const MPIOperation *, const MPIOperation *>>
      matched_pairs;

  for (const MPIOperation &op1 : all_operations_) {
    if (op1.kind != MPIOpKind::SEND_BLOCKING &&
        op1.kind != MPIOpKind::SEND_NONBLOCKING) {
      continue;
    }
    if (op1.tag < 0) {
      continue;
    }

    for (const MPIOperation &op2 : all_operations_) {
      if (op2.kind != MPIOpKind::RECV_BLOCKING &&
          op2.kind != MPIOpKind::RECV_NONBLOCKING) {
        continue;
      }
      if (!sameCommunicatorForProof(op1, op2)) {
        continue;
      }
      if (!rangesOverlap(op1.dest_rank_min, op1.dest_rank_max,
                         op2.source_rank_min, op2.source_rank_max)) {
        continue;
      }

      if (op2.tag >= 0 && op2.tag != op1.tag) {
        auto key = std::make_pair(op1.communicator_class_id,
                                  op2.communicator_class_id);
        matched_pairs[key] = {&op1, &op2};
      }
    }
  }

  for (const auto &pair : matched_pairs) {
    mismatches.emplace_back(pair.second.first->inst, pair.second.second->inst);
  }

  return mismatches;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findCountDatatypeMismatches() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> mismatches;

  std::set<std::pair<const Instruction *, const Instruction *>> added;

  for (const MPIOperation &send_op : all_operations_) {
    if (send_op.kind != MPIOpKind::SEND_BLOCKING &&
        send_op.kind != MPIOpKind::SEND_NONBLOCKING) {
      continue;
    }

    for (const MPIOperation &recv_op : all_operations_) {
      if (recv_op.kind != MPIOpKind::RECV_BLOCKING &&
          recv_op.kind != MPIOpKind::RECV_NONBLOCKING) {
        continue;
      }
      if (!sameCommunicatorForProof(send_op, recv_op)) {
        continue;
      }
      if (!rangesOverlap(send_op.dest_rank_min, send_op.dest_rank_max,
                         recv_op.source_rank_min, recv_op.source_rank_max)) {
        continue;
      }

      bool count_mismatch = false;
      bool datatype_mismatch = false;

      if (send_op.datatype && recv_op.datatype &&
          send_op.datatype != recv_op.datatype && send_op.datatype_size > 0 &&
          recv_op.datatype_size > 0 &&
          send_op.datatype_size != recv_op.datatype_size) {
        datatype_mismatch = true;
      }
      if (send_op.byte_length > 0 && recv_op.byte_length > 0 &&
          send_op.byte_length != recv_op.byte_length) {
        count_mismatch = true;
      }

      if (count_mismatch || datatype_mismatch) {
        auto pair = std::make_pair(send_op.inst, recv_op.inst);
        if (added.insert(pair).second) {
          mismatches.push_back(pair);
        }
      }
    }
  }

  return mismatches;
}

std::vector<const Instruction *> MPIProcessModel::findRankOutOfBounds() const {
  std::vector<const Instruction *> out_of_bounds;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::SEND_BLOCKING &&
        op.kind != MPIOpKind::SEND_NONBLOCKING &&
        op.kind != MPIOpKind::RECV_BLOCKING &&
        op.kind != MPIOpKind::RECV_NONBLOCKING) {
      continue;
    }

    if ((op.kind == MPIOpKind::SEND_BLOCKING ||
         op.kind == MPIOpKind::SEND_NONBLOCKING) &&
        op.dest_rank < -2) {
      out_of_bounds.push_back(op.inst);
      continue;
    }

    if ((op.kind == MPIOpKind::RECV_BLOCKING ||
         op.kind == MPIOpKind::RECV_NONBLOCKING) &&
        op.source_rank < -2) {
      out_of_bounds.push_back(op.inst);
    }
  }

  return out_of_bounds;
}

std::vector<RequestID> MPIProcessModel::findPersistentRequestLeaks() const {
  std::vector<RequestID> leaks;

  for (const auto &pair : persistent_request_templates_) {
    const NonBlockingOp &persistent_op = pair.second;
    if (persistent_op.completion_state == MPIRequestState::Pending ||
        persistent_op.completion_state == MPIRequestState::Created ||
        persistent_op.completion_state == MPIRequestState::Active) {
      leaks.push_back(pair.first);
    }
  }

  return leaks;
}

std::vector<const Instruction *>
MPIProcessModel::findCancelWithoutWait() const {
  std::vector<const Instruction *> issues;

  std::map<RequestID, const Instruction *> cancel_ops;
  std::set<RequestID> observed_after_cancel;

  for (const MPIOperation &op : all_operations_) {
    if (op.td_type == ThreadAPI::TD_MPI_CANCEL && op.request) {
      cancel_ops[op.request] = op.inst;
      continue;
    }

    if (!op.request) {
      continue;
    }

    if ((op.kind == MPIOpKind::WAIT || op.kind == MPIOpKind::TEST) &&
        cancel_ops.count(op.request)) {
      observed_after_cancel.insert(op.request);
    }
  }

  for (const auto &entry : cancel_ops) {
    if (!observed_after_cancel.count(entry.first)) {
      issues.push_back(entry.second);
    }
  }

  return issues;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findBufferOverlaps() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> overlaps;

  for (const MPIOperation &op : all_operations_) {
    if (op.td_type != ThreadAPI::TD_MPI_SENDRECV) {
      continue;
    }

    const CallBase *cb = dyn_cast<CallBase>(op.inst);
    if (!cb || cb->arg_size() < 11) {
      continue;
    }

    const Value *sendbuf = cb->getArgOperand(0);
    const Value *recvbuf = cb->getArgOperand(5);

    if (sendbuf && recvbuf && sendbuf == recvbuf) {
      overlaps.emplace_back(op.inst, op.inst);
    }
  }

  return overlaps;
}

std::vector<const Instruction *>
MPIProcessModel::findWildcardInCollective() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }

    if (op.source_rank == -1) {
      issues.push_back(op.inst);
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findInPlaceConflicts() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }

    if (op.td_type == ThreadAPI::TD_MPI_GATHER ||
        op.td_type == ThreadAPI::TD_MPI_ALLGATHER ||
        op.td_type == ThreadAPI::TD_MPI_ALLTOALL) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb && cb->arg_size() > 0) {
        const Value *sendbuf = cb->getArgOperand(0);
        if (isLikelyMPIInPlace(sendbuf)) {
          issues.push_back(op.inst);
        }
      }
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findNullHandles() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind == MPIOpKind::REQUEST_MANAGEMENT ||
        op.kind == MPIOpKind::COMM_MANAGEMENT) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb && cb->arg_size() > 0) {
        const Value *arg = cb->getArgOperand(0);
        if (isLikelyNullHandle(arg)) {
          issues.push_back(op.inst);
        }
      }
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findNegativeRoot() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }

    if (op.td_type == ThreadAPI::TD_MPI_BCAST ||
        op.td_type == ThreadAPI::TD_MPI_REDUCE ||
        op.td_type == ThreadAPI::TD_MPI_GATHER ||
        op.td_type == ThreadAPI::TD_MPI_SCATTER) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb) {
        int root_arg = -1;
        switch (op.td_type) {
        case ThreadAPI::TD_MPI_BCAST:
          root_arg = 3;
          break;
        case ThreadAPI::TD_MPI_REDUCE:
          root_arg = 5;
          break;
        case ThreadAPI::TD_MPI_GATHER:
        case ThreadAPI::TD_MPI_SCATTER:
          root_arg = 6;
          break;
        default:
          break;
        }
        if (root_arg >= 0 && static_cast<unsigned>(root_arg) < cb->arg_size()) {
          if (const auto *ci =
                  dyn_cast<ConstantInt>(cb->getArgOperand(root_arg))) {
            if (ci->getSExtValue() < 0) {
              issues.push_back(op.inst);
            }
          }
        }
      }
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findInvalidTags() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::SEND_BLOCKING &&
        op.kind != MPIOpKind::SEND_NONBLOCKING &&
        op.kind != MPIOpKind::RECV_BLOCKING &&
        op.kind != MPIOpKind::RECV_NONBLOCKING) {
      continue;
    }

    if (op.tag < 0) {
      issues.push_back(op.inst);
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findInvalidRanks() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::SEND_BLOCKING &&
        op.kind != MPIOpKind::SEND_NONBLOCKING &&
        op.kind != MPIOpKind::RECV_BLOCKING &&
        op.kind != MPIOpKind::RECV_NONBLOCKING) {
      continue;
    }

    if ((op.kind == MPIOpKind::SEND_BLOCKING ||
         op.kind == MPIOpKind::SEND_NONBLOCKING) &&
        op.dest_rank < -2) {
      issues.push_back(op.inst);
      continue;
    }

    if ((op.kind == MPIOpKind::RECV_BLOCKING ||
         op.kind == MPIOpKind::RECV_NONBLOCKING) &&
        !isMPIValidRankLikeValue(op.source_rank)) {
      issues.push_back(op.inst);
    }
  }

  return issues;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findTypeSizeMismatches() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> mismatches;
  std::set<std::pair<const Instruction *, const Instruction *>> added;

  for (const MPIOperation &send_op : all_operations_) {
    if (send_op.kind != MPIOpKind::SEND_BLOCKING &&
        send_op.kind != MPIOpKind::SEND_NONBLOCKING) {
      continue;
    }

    for (const MPIOperation &recv_op : all_operations_) {
      if (recv_op.kind != MPIOpKind::RECV_BLOCKING &&
          recv_op.kind != MPIOpKind::RECV_NONBLOCKING) {
        continue;
      }
      if (!sameCommunicatorForProof(send_op, recv_op)) {
        continue;
      }
      if (!rangesOverlap(send_op.dest_rank_min, send_op.dest_rank_max,
                         recv_op.source_rank_min, recv_op.source_rank_max)) {
        continue;
      }
      if (send_op.byte_length <= 0 || recv_op.byte_length <= 0) {
        continue;
      }
      if (send_op.byte_length == recv_op.byte_length) {
        continue;
      }
      auto pair = std::make_pair(send_op.inst, recv_op.inst);
      if (added.insert(pair).second) {
        mismatches.push_back(pair);
      }
    }
  }

  return mismatches;
}

std::vector<const Instruction *> MPIProcessModel::findDestroyNullComm() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COMM_MANAGEMENT) {
      continue;
    }
    if (op.td_type == ThreadAPI::TD_MPI_COMM_FREE) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb && cb->arg_size() > 0) {
        const Value *comm = cb->getArgOperand(0);
        if (isLikelyNullHandle(comm)) {
          issues.push_back(op.inst);
        }
      }
    }
  }

  return issues;
}

std::vector<const Instruction *>
MPIProcessModel::findRequestFreeAfterWait() const {
  std::vector<const Instruction *> issues;
  std::set<RequestID> completed_requests;

  for (const MPIOperation &op : all_operations_) {
    if (!op.request) {
      continue;
    }

    if ((op.kind == MPIOpKind::WAIT || op.kind == MPIOpKind::TEST ||
         op.kind == MPIOpKind::REQUEST_MANAGEMENT) &&
        (op.request_state == RequestCompletionState::MustComplete ||
         op.request_state == RequestCompletionState::Terminal ||
         op.request_state == RequestCompletionState::Freed)) {
      completed_requests.insert(op.request);
    }

    if (op.td_type == ThreadAPI::TD_MPI_REQUEST_FREE &&
        completed_requests.count(op.request)) {
      issues.push_back(op.inst);
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findInPlaceWrongOp() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }

    if (op.td_type == ThreadAPI::TD_MPI_REDUCE ||
        op.td_type == ThreadAPI::TD_MPI_SCAN ||
        op.td_type == ThreadAPI::TD_MPI_BCAST ||
        op.td_type == ThreadAPI::TD_MPI_SCATTER) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb && cb->arg_size() > 0) {
        const Value *sendbuf = cb->getArgOperand(0);
        if (isLikelyMPIInPlace(sendbuf)) {
          issues.push_back(op.inst);
        }
      }
    }
  }

  return issues;
}

} // namespace mpi
