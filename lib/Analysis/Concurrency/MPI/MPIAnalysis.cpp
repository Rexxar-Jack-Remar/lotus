/**
 * @file MPIAnalysis.cpp
 * @brief MPI Program Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPIAnalysis.h"

#include "Analysis/Concurrency/MPI/MPIModel.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"

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

std::string normalizeMPIName(StringRef raw_name) {
  StringRef name = raw_name;
  if (name.startswith("\01")) {
    name = name.drop_front();
  }
  if (name.startswith("PMPI_")) {
    return ("MPI_" + name.drop_front(5)).str();
  }
  return name.str();
}

bool isMPIWildcardValue(int value) {
  // Handle common encodings seen in IR for MPI_ANY_* constants.
  return value == -1 || value == -2;
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

// ============================================================================
// MPIProcessModel Implementation
// ============================================================================

MPIOpKind MPIProcessModel::classifyOperation(const Instruction *inst,
                                             ThreadAPI::TD_TYPE type) const {
  switch (type) {
  case ThreadAPI::TD_MPI_INIT:
    return MPIOpKind::INIT;
  case ThreadAPI::TD_MPI_FINALIZE:
    return MPIOpKind::FINALIZE;
  case ThreadAPI::TD_MPI_SEND:
    return MPIOpKind::SEND_BLOCKING;
  case ThreadAPI::TD_MPI_RECV:
    return MPIOpKind::RECV_BLOCKING;
  case ThreadAPI::TD_MPI_PROBE:
    return MPIOpKind::PROBE_BLOCKING;
  case ThreadAPI::TD_MPI_SENDRECV:
    return MPIOpKind::UNKNOWN;
  case ThreadAPI::TD_MPI_ISEND:
    return MPIOpKind::SEND_NONBLOCKING;
  case ThreadAPI::TD_MPI_IRECV:
    return MPIOpKind::RECV_NONBLOCKING;
  case ThreadAPI::TD_MPI_IPROBE:
    return MPIOpKind::PROBE_NONBLOCKING;
  case ThreadAPI::TD_MPI_WAIT:
  case ThreadAPI::TD_MPI_WAITALL:
  case ThreadAPI::TD_MPI_WAITANY:
  case ThreadAPI::TD_MPI_WAITSOME:
    return MPIOpKind::WAIT;
  case ThreadAPI::TD_MPI_TEST:
  case ThreadAPI::TD_MPI_TESTALL:
  case ThreadAPI::TD_MPI_TESTANY:
  case ThreadAPI::TD_MPI_TESTSOME:
    return MPIOpKind::TEST;
  case ThreadAPI::TD_MPI_BARRIER:
    return thread_api_->isNonBlockingMPIBarrier(inst)
               ? MPIOpKind::BARRIER_NONBLOCKING
               : MPIOpKind::BARRIER_BLOCKING;
  case ThreadAPI::TD_MPI_BCAST:
  case ThreadAPI::TD_MPI_SCATTER:
  case ThreadAPI::TD_MPI_GATHER:
  case ThreadAPI::TD_MPI_ALLGATHER:
  case ThreadAPI::TD_MPI_ALLTOALL:
  case ThreadAPI::TD_MPI_REDUCE:
  case ThreadAPI::TD_MPI_ALLREDUCE:
  case ThreadAPI::TD_MPI_REDUCE_SCATTER:
  case ThreadAPI::TD_MPI_SCAN:
    return thread_api_->isNonBlockingMPICollective(inst)
               ? MPIOpKind::COLLECTIVE_NONBLOCKING
               : MPIOpKind::COLLECTIVE_BLOCKING;
  case ThreadAPI::TD_MPI_WIN_CREATE:
  case ThreadAPI::TD_MPI_WIN_FREE:
    return MPIOpKind::RMA_WINDOW;
  case ThreadAPI::TD_MPI_PUT:
  case ThreadAPI::TD_MPI_GET:
  case ThreadAPI::TD_MPI_ACCUMULATE:
    return MPIOpKind::RMA_DATA;
  case ThreadAPI::TD_MPI_WIN_FENCE:
  case ThreadAPI::TD_MPI_WIN_LOCK:
  case ThreadAPI::TD_MPI_WIN_UNLOCK:
  case ThreadAPI::TD_MPI_WIN_FLUSH:
  case ThreadAPI::TD_MPI_WIN_SYNC:
  case ThreadAPI::TD_MPI_WIN_POST:
  case ThreadAPI::TD_MPI_WIN_START:
  case ThreadAPI::TD_MPI_WIN_COMPLETE:
  case ThreadAPI::TD_MPI_WIN_WAIT:
  case ThreadAPI::TD_MPI_WIN_TEST:
    return MPIOpKind::RMA_SYNC;
  case ThreadAPI::TD_MPI_COMM_DUP:
  case ThreadAPI::TD_MPI_COMM_SPLIT:
  case ThreadAPI::TD_MPI_COMM_CREATE:
  case ThreadAPI::TD_MPI_COMM_FREE:
    return MPIOpKind::COMM_MANAGEMENT;
  case ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT:
  case ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT:
  case ThreadAPI::TD_MPI_REQUEST_START:
  case ThreadAPI::TD_MPI_REQUEST_FREE:
  case ThreadAPI::TD_MPI_CANCEL:
    return MPIOpKind::REQUEST_MANAGEMENT;
  default:
    return MPIOpKind::UNKNOWN;
  }
}

bool MPIProcessModel::tryGetConstantInt(const Value *value, int &out) const {
  const auto *ci = dyn_cast_or_null<ConstantInt>(value);
  if (!ci) {
    return false;
  }
  out = ci->getSExtValue();
  return true;
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

size_t MPIProcessModel::assignCommunicatorClass(CommunicatorID canonical) {
  if (!canonical) {
    return 0;
  }
  canonical = canonical->stripPointerCasts();
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

void MPIProcessModel::annotateRankConstraints(MPIOperation &op) const {
  if (!rank_analysis_ || !op.inst) {
    return;
  }

  MPI::RankExpr rank = rank_analysis_->getRankAtInstruction(op.inst);
  op.process_rank = rank;
  switch (rank.kind) {
  case MPI::RankExpr::Concrete:
    op.protocol_reachability = ProtocolReachability::SomeRanks;
    op.rank_path_summary = ("rank==" + std::to_string(rank.concrete_value));
    break;
  case MPI::RankExpr::Range:
    op.protocol_reachability = ProtocolReachability::SomeRanks;
    op.rank_path_summary = ("rank in [" + std::to_string(rank.range_min) +
                            ", " + std::to_string(rank.range_max) + "]");
    break;
  case MPI::RankExpr::Symbolic:
    op.protocol_reachability = ProtocolReachability::AllRanks;
    op.rank_path_summary = "rank symbolic";
    break;
  case MPI::RankExpr::Unknown:
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
  int datatype = 0;
  if (!tryReadScalarInt(datatype_arg, datatype, context)) {
    return -1;
  }
  switch (datatype) {
  case 0:
    return 1;
  case 1:
    return 2;
  case 2:
    return 4;
  case 3:
    return 8;
  default:
    return -1;
  }
}

void MPIProcessModel::extractPointToPointDetails(MPIOperation &op,
                                                 const CallBase *cb) {
  if (!cb) {
    return;
  }

  unsigned num_args = cb->arg_size();
  const bool send_like = op.kind == MPIOpKind::SEND_BLOCKING ||
                         op.kind == MPIOpKind::SEND_NONBLOCKING ||
                         op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT;
  const bool recv_like = op.kind == MPIOpKind::RECV_BLOCKING ||
                         op.kind == MPIOpKind::RECV_NONBLOCKING ||
                         op.td_type == ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT;

  if (send_like) {
    if (num_args >= 6) {
      int value = -1;
      if (tryGetConstantInt(cb->getArgOperand(3), value)) {
        op.dest_rank = value;
      }
      if (tryGetConstantInt(cb->getArgOperand(4), value)) {
        op.tag = value;
      }
      op.communicator = canonicalizeCommunicator(cb->getArgOperand(5));
    }
    if (op.kind == MPIOpKind::SEND_NONBLOCKING && num_args >= 7) {
      op.request = cb->getArgOperand(6);
    }
    if (op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT && num_args >= 7) {
      op.request = cb->getArgOperand(6);
    }
    return;
  }

  if (recv_like) {
    if (num_args >= 6) {
      int value = -1;
      if (tryGetConstantInt(cb->getArgOperand(3), value)) {
        op.source_rank = value;
      }
      if (tryGetConstantInt(cb->getArgOperand(4), value)) {
        op.tag = value;
      }
      op.communicator = canonicalizeCommunicator(cb->getArgOperand(5));
    }
    if (op.kind == MPIOpKind::RECV_NONBLOCKING && num_args >= 7) {
      op.request = cb->getArgOperand(6);
    }
    if (op.td_type == ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT && num_args >= 7) {
      op.request = cb->getArgOperand(6);
    }
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
    // MPI_Sendrecv(..., dest(3), sendtag(4), ..., source(8), recvtag(9),
    // comm(10), ...)
    if (op.kind == MPIOpKind::SEND_BLOCKING) {
      if (tryGetConstantInt(cb->getArgOperand(3), value)) {
        op.dest_rank = value;
      }
      if (tryGetConstantInt(cb->getArgOperand(4), value)) {
        op.tag = value;
      }
    } else {
      if (tryGetConstantInt(cb->getArgOperand(8), value)) {
        op.source_rank = value;
      }
      if (tryGetConstantInt(cb->getArgOperand(9), value)) {
        op.tag = value;
      }
    }
    op.communicator = canonicalizeCommunicator(cb->getArgOperand(10));
    return;
  }

  if (num_args >= 8) {
    // MPI_Sendrecv_replace(..., dest(3), sendtag(4), source(5), recvtag(6),
    // comm(7), ...)
    if (op.kind == MPIOpKind::SEND_BLOCKING) {
      if (tryGetConstantInt(cb->getArgOperand(3), value)) {
        op.dest_rank = value;
      }
      if (tryGetConstantInt(cb->getArgOperand(4), value)) {
        op.tag = value;
      }
    } else {
      if (tryGetConstantInt(cb->getArgOperand(5), value)) {
        op.source_rank = value;
      }
      if (tryGetConstantInt(cb->getArgOperand(6), value)) {
        op.tag = value;
      }
    }
    op.communicator = canonicalizeCommunicator(cb->getArgOperand(7));
  }
}

void MPIProcessModel::extractProbeDetails(MPIOperation &op,
                                          const CallBase *cb) const {
  if (!cb || cb->arg_size() < 3) {
    return;
  }
  int value = -1;
  if (tryGetConstantInt(cb->getArgOperand(0), value)) {
    op.source_rank = value;
  }
  if (tryGetConstantInt(cb->getArgOperand(1), value)) {
    op.tag = value;
  }
  op.communicator = canonicalizeCommunicator(cb->getArgOperand(2));
}

void MPIProcessModel::extractCollectiveDetails(MPIOperation &op,
                                               const CallBase *cb) const {
  if (!cb || cb->arg_size() == 0) {
    return;
  }
  const Function *callee = cb->getCalledFunction();
  if (!callee) {
    return;
  }
  const bool nonblocking = op.kind == MPIOpKind::BARRIER_NONBLOCKING ||
                           op.kind == MPIOpKind::COLLECTIVE_NONBLOCKING;
  unsigned comm_idx = cb->arg_size() - 1;
  if (nonblocking && cb->arg_size() >= 2) {
    comm_idx = cb->arg_size() - 2;
  }
  if (comm_idx < cb->arg_size()) {
    op.communicator = canonicalizeCommunicator(cb->getArgOperand(comm_idx));
  }
  if (nonblocking) {
    op.request = cb->getArgOperand(cb->arg_size() - 1);
  }
}

void MPIProcessModel::extractRequestDetails(MPIOperation &op,
                                            const CallBase *cb) const {
  if (!cb) {
    return;
  }
  unsigned num_args = cb->arg_size();
  switch (op.td_type) {
  case ThreadAPI::TD_MPI_WAIT:
  case ThreadAPI::TD_MPI_TEST:
    if (num_args >= 1) {
      op.request = cb->getArgOperand(0);
    }
    break;
  case ThreadAPI::TD_MPI_WAITALL:
  case ThreadAPI::TD_MPI_WAITANY:
  case ThreadAPI::TD_MPI_WAITSOME:
  case ThreadAPI::TD_MPI_TESTALL:
  case ThreadAPI::TD_MPI_TESTANY:
  case ThreadAPI::TD_MPI_TESTSOME:
    if (num_args >= 2) {
      op.request = cb->getArgOperand(1);
    }
    break;
  case ThreadAPI::TD_MPI_REQUEST_FREE:
  case ThreadAPI::TD_MPI_CANCEL:
    if (num_args >= 1) {
      op.request = cb->getArgOperand(0);
    }
    break;
  case ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT:
    if (num_args >= 7) {
      op.request = cb->getArgOperand(6);
    }
    break;
  case ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT:
    if (num_args >= 7) {
      op.request = cb->getArgOperand(6);
    }
    break;
  case ThreadAPI::TD_MPI_REQUEST_START:
    if (num_args >= 2) {
      op.request = cb->getArgOperand(1);
    } else if (num_args >= 1) {
      op.request = cb->getArgOperand(0);
    }
    break;
  default:
    break;
  }
}

void MPIProcessModel::extractRMAWindowDetails(MPIOperation &op,
                                              const CallBase *cb,
                                              StringRef callee_name) const {
  if (!cb) {
    return;
  }
  if (callee_name.equals("MPI_Win_create") ||
      callee_name.equals("MPI_Win_allocate") ||
      callee_name.equals("MPI_Win_allocate_shared") ||
      callee_name.equals("MPI_Win_create_dynamic")) {
    if (cb->arg_size() >= 2) {
      op.communicator =
          canonicalizeCommunicator(cb->getArgOperand(cb->arg_size() - 2));
      op.window = cb->getArgOperand(cb->arg_size() - 1);
    }
    return;
  }
  if (callee_name.equals("MPI_Win_free") && cb->arg_size() >= 1) {
    op.window = cb->getArgOperand(0);
  }
}

void MPIProcessModel::extractRMADataDetails(MPIOperation &op,
                                            const CallBase *cb,
                                            StringRef callee_name) const {
  if (!cb) {
    return;
  }
  unsigned num_args = cb->arg_size();
  int value = -1;
  auto setByteLength = [&](unsigned count_idx, unsigned datatype_idx) {
    int count = 0;
    if (!tryReadScalarInt(cb->getArgOperand(count_idx), count, op.inst)) {
      return;
    }
    int64_t extent =
        getDatatypeExtent(cb->getArgOperand(datatype_idx), op.inst);
    if (extent <= 0) {
      return;
    }
    op.byte_length = static_cast<int64_t>(count) * extent;
  };
  if ((callee_name.equals("MPI_Put") || callee_name.equals("MPI_Rput") ||
       callee_name.equals("MPI_Get") || callee_name.equals("MPI_Rget") ||
       callee_name.equals("MPI_Accumulate") ||
       callee_name.equals("MPI_Raccumulate")) &&
      num_args >= 8) {
    setByteLength(1, 2);
    if (tryGetConstantInt(cb->getArgOperand(3), value)) {
      op.target_rank = value;
    }
    if (const auto *disp = dyn_cast<ConstantInt>(cb->getArgOperand(4))) {
      op.target_disp = disp->getSExtValue();
    }
    op.window = cb->getArgOperand(7);
    return;
  }

  if ((callee_name.equals("MPI_Get_accumulate") ||
       callee_name.equals("MPI_Rget_accumulate")) &&
      num_args >= 12) {
    setByteLength(4, 5);
    if (tryGetConstantInt(cb->getArgOperand(6), value)) {
      op.target_rank = value;
    }
    if (const auto *disp = dyn_cast<ConstantInt>(cb->getArgOperand(7))) {
      op.target_disp = disp->getSExtValue();
    }
    op.window = cb->getArgOperand(11);
    return;
  }

  if (callee_name.equals("MPI_Fetch_and_op") && num_args >= 7) {
    op.byte_length = 1;
    if (tryGetConstantInt(cb->getArgOperand(3), value)) {
      op.target_rank = value;
    }
    if (const auto *disp = dyn_cast<ConstantInt>(cb->getArgOperand(4))) {
      op.target_disp = disp->getSExtValue();
    }
    op.window = cb->getArgOperand(6);
    return;
  }

  if (callee_name.equals("MPI_Compare_and_swap") && num_args >= 7) {
    op.byte_length = 1;
    if (tryGetConstantInt(cb->getArgOperand(4), value)) {
      op.target_rank = value;
    }
    if (const auto *disp = dyn_cast<ConstantInt>(cb->getArgOperand(5))) {
      op.target_disp = disp->getSExtValue();
    }
    op.window = cb->getArgOperand(6);
    return;
  }

  if (num_args >= 8) {
    setByteLength(1, 2);
    if (tryGetConstantInt(cb->getArgOperand(3), value)) {
      op.target_rank = value;
    }
    if (const auto *disp = dyn_cast<ConstantInt>(cb->getArgOperand(4))) {
      op.target_disp = disp->getSExtValue();
    }
    op.window = cb->getArgOperand(7);
  }
}

void MPIProcessModel::extractRMASyncDetails(MPIOperation &op,
                                            const CallBase *cb,
                                            StringRef callee_name) const {
  if (!cb) {
    return;
  }
  unsigned num_args = cb->arg_size();
  int value = -1;
  if (callee_name.equals("MPI_Win_fence")) {
    if (num_args >= 2) {
      op.window = cb->getArgOperand(1);
    }
    return;
  }
  if (callee_name.equals("MPI_Win_lock")) {
    if (num_args >= 4) {
      if (tryGetConstantInt(cb->getArgOperand(1), value)) {
        op.target_rank = value;
      }
      op.window = cb->getArgOperand(3);
    }
    return;
  }
  if (callee_name.equals("MPI_Win_lock_all")) {
    if (num_args >= 2) {
      op.window = cb->getArgOperand(1);
    }
    return;
  }
  if (callee_name.equals("MPI_Win_unlock") ||
      callee_name.equals("MPI_Win_flush") ||
      callee_name.equals("MPI_Win_flush_local")) {
    if (num_args >= 2) {
      if (tryGetConstantInt(cb->getArgOperand(0), value)) {
        op.target_rank = value;
      }
      op.window = cb->getArgOperand(1);
    }
    return;
  }
  if (callee_name.equals("MPI_Win_unlock_all") ||
      callee_name.equals("MPI_Win_flush_all") ||
      callee_name.equals("MPI_Win_flush_local_all")) {
    if (num_args >= 1) {
      op.window = cb->getArgOperand(0);
    }
    return;
  }
  if (callee_name.equals("MPI_Win_sync") ||
      callee_name.equals("MPI_Win_complete") ||
      callee_name.equals("MPI_Win_wait") ||
      callee_name.equals("MPI_Win_test")) {
    if (num_args >= 1) {
      op.window = cb->getArgOperand(0);
    }
    return;
  }
  if (callee_name.equals("MPI_Win_post") ||
      callee_name.equals("MPI_Win_start")) {
    if (num_args >= 3) {
      op.window = cb->getArgOperand(2);
    }
  }
}

void MPIProcessModel::extractOperationDetails(MPIOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb) {
    return;
  }
  const Function *callee = cb->getCalledFunction();
  const std::string normalized_name =
      callee ? normalizeMPIName(callee->getName()) : std::string();
  StringRef callee_name = normalized_name;

  switch (op.kind) {
  case MPIOpKind::SEND_BLOCKING:
  case MPIOpKind::RECV_BLOCKING:
  case MPIOpKind::SEND_NONBLOCKING:
  case MPIOpKind::RECV_NONBLOCKING:
    if (op.td_type == ThreadAPI::TD_MPI_SENDRECV) {
      extractSendrecvDetails(op, cb);
    } else {
      extractPointToPointDetails(op, cb);
    }
    break;
  case MPIOpKind::PROBE_BLOCKING:
  case MPIOpKind::PROBE_NONBLOCKING:
    extractProbeDetails(op, cb);
    break;
  case MPIOpKind::WAIT:
  case MPIOpKind::TEST:
    extractRequestDetails(op, cb);
    break;
  case MPIOpKind::BARRIER_BLOCKING:
  case MPIOpKind::BARRIER_NONBLOCKING:
  case MPIOpKind::COLLECTIVE_BLOCKING:
  case MPIOpKind::COLLECTIVE_NONBLOCKING:
    extractCollectiveDetails(op, cb);
    break;
  case MPIOpKind::REQUEST_MANAGEMENT:
    if (op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT ||
        op.td_type == ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT) {
      extractPointToPointDetails(op, cb);
    }
    extractRequestDetails(op, cb);
    break;
  case MPIOpKind::RMA_WINDOW:
    extractRMAWindowDetails(op, cb, callee_name);
    break;
  case MPIOpKind::RMA_DATA:
    extractRMADataDetails(op, cb, callee_name);
    break;
  case MPIOpKind::RMA_SYNC:
    extractRMASyncDetails(op, cb, callee_name);
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
  deferred_lowering_stats_.clear();
  rank_analysis_ = std::make_unique<MPI::MPIRankAnalysis>(module_);
  rank_analysis_->analyze();

  // Iterate through all instructions in the module
  for (Function &F : module_) {
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      Instruction *I = &*II;

      const Function *callee = thread_api_->getCallee(I);
      if (!callee)
        continue;

      ThreadAPI::TD_TYPE type = thread_api_->getType(callee);
      if (type == ThreadAPI::TD_DUMMY)
        continue;

      if (type == ThreadAPI::TD_MPI_SENDRECV) {
        MPIOperation send_op(I, MPIOpKind::SEND_BLOCKING, type);
        extractOperationDetails(send_op);
        annotateRankConstraints(send_op);
        if (send_op.communicator) {
          send_op.communicator_class_id = assignCommunicatorClass(
              canonicalizeCommunicator(send_op.communicator));
        }
        all_operations_.push_back(send_op);
        ++operation_kind_counts_[send_op.kind];

        MPIOperation recv_op(I, MPIOpKind::RECV_BLOCKING, type);
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

      // Check if this is an MPI operation
      MPIOpKind kind = classifyOperation(I, type);
      if (kind == MPIOpKind::UNKNOWN)
        continue;

      // Create operation record
      MPIOperation op(I, kind, type);
      extractOperationDetails(op);
      annotateRankConstraints(op);
      if (op.communicator) {
        op.communicator_class_id =
            assignCommunicatorClass(canonicalizeCommunicator(op.communicator));
      }

      if (kind == MPIOpKind::COMM_MANAGEMENT) {
        if (const auto *cb = dyn_cast<CallBase>(I)) {
          const std::string normalized_name =
              callee ? normalizeMPIName(callee->getName()) : std::string();
          StringRef callee_name = normalized_name;
          const Value *root =
              cb->arg_size() >= 1 ? cb->getArgOperand(0) : nullptr;
          if ((callee_name.equals("MPI_Comm_dup") ||
               callee_name.equals("MPI_Comm_dup_with_info") ||
               callee_name.equals("MPI_Comm_idup")) &&
              cb->arg_size() >= 2) {
            registerCommunicatorAlias(cb->getArgOperand(1), root);
          } else if (callee_name.equals("MPI_Comm_split") &&
                     cb->arg_size() >= 4) {
            registerCommunicatorAlias(cb->getArgOperand(3), root);
          } else if (callee_name.equals("MPI_Comm_split_type") &&
                     cb->arg_size() >= 5) {
            registerCommunicatorAlias(cb->getArgOperand(4), root);
          } else if (callee_name.equals("MPI_Comm_create") &&
                     cb->arg_size() >= 3) {
            registerCommunicatorAlias(cb->getArgOperand(2), root);
          } else if (callee_name.equals("MPI_Comm_create_group") &&
                     cb->arg_size() >= 4) {
            registerCommunicatorAlias(cb->getArgOperand(3), root);
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
          persistent_op.completion_state = RequestCompletionState::Pending;
          persistent_op.peer_rank =
              op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT
                  ? op.dest_rank
                  : op.source_rank;
          persistent_op.tag = op.tag;
          persistent_op.comm = op.communicator;
          persistent_request_templates_[op.request] = persistent_op;
        }
      }

      // Track non-blocking operations
      if (kind == MPIOpKind::SEND_NONBLOCKING ||
          kind == MPIOpKind::RECV_NONBLOCKING ||
          kind == MPIOpKind::BARRIER_NONBLOCKING ||
          kind == MPIOpKind::COLLECTIVE_NONBLOCKING) {
        if (op.request) {
          NonBlockingOp nbOp;
          nbOp.issue_inst = I;
          nbOp.request = op.request;
          nbOp.completion_state = RequestCompletionState::Pending;
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

  // Match non-blocking operations with their completions
  matchNonBlockingOps();

  if (!deferred_lowering_stats_.empty()) {
    errs() << "MPI deferred lowering:";
    for (const auto &entry : deferred_lowering_stats_) {
      errs() << " " << entry.first << "=" << entry.second;
    }
    errs() << "\n";
  }
}

size_t MPIAnalysis::getProtocolDiagnosticCount(StringRef key) const {
  const auto &diagnostics = collective_analysis_.getProtocolDiagnostics();
  auto it = diagnostics.find(key.str());
  return it != diagnostics.end() ? it->second : 0;
}

size_t MPIAnalysis::getOperationCount(MPIOpKind kind) const {
  const auto &counts = process_model_.getOperationKindCounts();
  auto it = counts.find(kind);
  return it != counts.end() ? it->second : 0;
}

size_t MPIAnalysis::getTrackedWindowCount() const {
  return rma_analysis_.getTrackedWindowCount();
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
        active_op.completion_state = RequestCompletionState::Pending;
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
        it->second.completion_state = RequestCompletionState::Terminal;
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
    auto markCompleted = [&](RequestID request, RequestCompletionState state) {
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
            if (it->second.completion_state ==
                RequestCompletionState::Pending) {
              it->second.completion_state = RequestCompletionState::MayComplete;
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
      markCompleted(requests.front(), RequestCompletionState::MustComplete);
      break;
    case ThreadAPI::TD_MPI_TEST: {
      bool test_true = false;
      bool flag_unknown = false;
      if (cb && cb->arg_size() >= 2) {
        int flag = 0;
        if (tryReadScalarInt(cb->getArgOperand(1), flag, op.inst)) {
          test_true = flag != 0;
        } else {
          deferred_lowering_stats_["test_unknown_flag"]++;
          flag_unknown = true;
        }
      } else {
        deferred_lowering_stats_["test_unknown_flag"]++;
        flag_unknown = true;
      }
      if (test_true) {
        markCompleted(requests.front(), RequestCompletionState::MustComplete);
      } else if (flag_unknown) {
        markCompleted(requests.front(), RequestCompletionState::MayComplete);
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
          markCompleted(request, RequestCompletionState::MustComplete);
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
                      RequestCompletionState::MustComplete);
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
          deferred_lowering_stats_["testany_unknown_flag"]++;
        }
        if (tryReadScalarInt(cb->getArgOperand(2), selected, op.inst)) {
          selected_known = true;
        } else {
          deferred_lowering_stats_["testany_unknown_index"]++;
        }
      } else {
        deferred_lowering_stats_["testany_unknown_flag"]++;
      }
      if (testany_true && selected_known && selected >= 0 &&
          static_cast<size_t>(selected) < requests.size()) {
        markCompleted(requests[static_cast<size_t>(selected)],
                      RequestCompletionState::MustComplete);
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
                      RequestCompletionState::MustComplete);
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
                      RequestCompletionState::MustComplete);
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
  // One must be send, other must be recv
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

  // Check if ranks match (send.dest == recv.source process)
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

  // Check if tags match
  if (!isMPIWildcardValue(send.tag) && !isMPIWildcardValue(recv.tag) &&
      send.tag != recv.tag) {
    return MPICommunicationMatch::NoMatch;
  }
  if (isMPIWildcardValue(send.tag) || isMPIWildcardValue(recv.tag) ||
      send.tag < 0 || recv.tag < 0) {
    precise = false;
  }

  // Check if communicators match
  if (send.communicator_class_id != 0 && recv.communicator_class_id != 0 &&
      send.communicator_class_id == recv.communicator_class_id) {
    // Proven same communicator class, continue with rank/tag checks only.
  } else if (send.communicator && recv.communicator &&
             !communicatorsMayAlias(send.communicator, recv.communicator)) {
    return MPICommunicationMatch::NoMatch;
  } else {
    precise = false;
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
    if (pair.second.completion_state == RequestCompletionState::Pending) {
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

// ============================================================================
// MPICollectiveAnalysis Implementation
// ============================================================================

void MPICollectiveAnalysis::analyzeCollectives() {
  collective_calls_.clear();
  protocol_diagnostics_.clear();
  std::map<std::pair<size_t, const llvm::Function *>, size_t>
      sequence_by_function_and_comm;

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
      call.function = op.function;
      call.sequence_index = sequence_by_function_and_comm[{
          op.communicator_class_id, op.function}]++;
      protocol_diagnostics_["collective_slots_tracked"]++;
      if (op.protocol_reachability != ProtocolReachability::AllRanks) {
        protocol_diagnostics_["collective_partial_reachability"]++;
      }

      // Extract root rank for operations that need it
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

      collective_calls_.push_back(call);
    }
  }
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
  // Calls on different or unknown communicators are not comparable here.
  // We only prove incompatibility when both are known to be in the same
  // communicator class.
  if (!c1.comm || !c2.comm) {
    return true;
  }
  if (c1.communicator_class_id != 0 && c2.communicator_class_id != 0 &&
      c1.communicator_class_id == c2.communicator_class_id) {
    // Proven same communicator class.
  } else if (!communicatorsMayAlias(c1.comm, c2.comm)) {
    return true;
  }

  // Same communicator must use compatible collective kinds.
  if (c1.type != c2.type)
    return false;

  // For rooted collectives, check root
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

  std::set<const Function *> collective_functions;
  for (const CollectiveCall &call : collective_calls_) {
    if (call.function) {
      collective_functions.insert(call.function);
    }
  }

  const bool single_function_model = collective_functions.size() <= 1;
  for (size_t i = 0; i < collective_calls_.size(); ++i) {
    for (size_t j = i + 1; j < collective_calls_.size(); ++j) {
      const CollectiveCall &c1 = collective_calls_[i];
      const CollectiveCall &c2 = collective_calls_[j];

      if (single_function_model) {
        if ((!communicatorsMayAlias(c1.comm, c2.comm) &&
             !(c1.communicator_class_id != 0 &&
               c1.communicator_class_id == c2.communicator_class_id)) ||
            c1.sequence_index != c2.sequence_index) {
          continue;
        }
      } else {
        if (c1.function == c2.function) {
          continue;
        }
        if (c1.sequence_index != c2.sequence_index ||
            (!communicatorsMayAlias(c1.comm, c2.comm) &&
             !(c1.communicator_class_id != 0 &&
               c1.communicator_class_id == c2.communicator_class_id))) {
          continue;
        }
      }

      if (!areCollectivesCompatible(c1, c2)) {
        protocol_diagnostics_["collective_mismatch_pairs"]++;
        mismatches.emplace_back(c1, c2);
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
  rank_analysis.analyze();

  for (const CollectiveCall &call : collective_calls_) {
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

// ============================================================================
// MPIRMAAnalysis Implementation
// ============================================================================

void MPIRMAAnalysis::analyzeRMA() {
  windows_.clear();
  rma_operations_.clear();

  // First pass: identify windows
  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.td_type == ThreadAPI::TD_MPI_WIN_CREATE) {
      RMAWindow window;
      window.window = nullptr; // Would need to extract from call
      window.create_inst = op.inst;

      // Extract window handle (typically last argument)
      const CallBase *CB = dyn_cast<CallBase>(op.inst);
      if (CB && CB->arg_size() > 0) {
        window.window = CB->getArgOperand(CB->arg_size() - 1);
        windows_[window.window] = window;
      }
    } else if (op.td_type == ThreadAPI::TD_MPI_WIN_FREE) {
      const CallBase *CB = dyn_cast<CallBase>(op.inst);
      if (CB && CB->arg_size() > 0) {
        WindowID win = CB->getArgOperand(0);
        auto it = windows_.find(win);
        if (it != windows_.end()) {
          it->second.free_inst = op.inst;
        }
      }
    }
  }

  struct PendingEpoch {
    SyncModel model = SyncModel::NONE;
    const Instruction *start = nullptr;
    std::vector<size_t> op_indices;
    size_t epoch_id = 0;
  };

  std::map<std::pair<const Function *, WindowID>, PendingEpoch> pending_epochs;
  std::map<std::pair<const Function *, WindowID>, const Instruction *>
      last_fence_by_window;
  size_t next_epoch_id = 1;

  // Second pass: collect RMA operations and sync
  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.kind == MPIOpKind::RMA_DATA) {
      RMAOperation rma_op;
      rma_op.inst = op.inst;
      rma_op.function = op.function;
      rma_op.window = op.window;
      rma_op.target_rank = op.target_rank;
      rma_op.target_rank_min = op.target_rank_min;
      rma_op.target_rank_max = op.target_rank_max;
      rma_op.target_disp = op.target_disp;
      rma_op.byte_length = op.byte_length;
      rma_op.rma_epoch_kind = RMAEpochKind::Access;

      size_t op_index = rma_operations_.size();
      rma_operations_.push_back(rma_op);

      auto key = std::make_pair(op.function, op.window);
      auto epoch_it = pending_epochs.find(key);
      if (epoch_it != pending_epochs.end() &&
          epoch_it->second.model != SyncModel::NONE) {
        epoch_it->second.op_indices.push_back(op_index);
      }

      auto it = windows_.find(op.window);
      if (it != windows_.end()) {
        if (op.td_type == ThreadAPI::TD_MPI_PUT) {
          it->second.put_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_GET) {
          it->second.get_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_ACCUMULATE) {
          it->second.accumulate_ops.insert(op.inst);
        }
      }
    } else if (op.kind == MPIOpKind::RMA_SYNC) {
      auto it = windows_.find(op.window);
      if (it != windows_.end()) {
        if (op.td_type == ThreadAPI::TD_MPI_WIN_FENCE) {
          it->second.fence_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_LOCK) {
          it->second.lock_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_UNLOCK) {
          it->second.unlock_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_FLUSH) {
          it->second.flush_ops.insert(op.inst);
        }
      }

      auto key = std::make_pair(op.function, op.window);
      PendingEpoch &epoch = pending_epochs[key];
      switch (op.td_type) {
      case ThreadAPI::TD_MPI_WIN_FENCE: {
        auto fence_it = last_fence_by_window.find(key);
        if (fence_it != last_fence_by_window.end()) {
          for (size_t idx : epoch.op_indices) {
            rma_operations_[idx].sync_model = SyncModel::FENCE;
            rma_operations_[idx].sync_start = fence_it->second;
            rma_operations_[idx].sync_end = op.inst;
            rma_operations_[idx].epoch_id = epoch.epoch_id;
            rma_operations_[idx].synchronization_proof =
                concurrency::ProofStrength::Must;
          }
          epoch.op_indices.clear();
        }
        epoch.model = SyncModel::FENCE;
        epoch.start = op.inst;
        epoch.epoch_id = next_epoch_id++;
        last_fence_by_window[key] = op.inst;
        break;
      }
      case ThreadAPI::TD_MPI_WIN_LOCK:
        epoch.model = SyncModel::LOCK_UNLOCK;
        epoch.start = op.inst;
        epoch.epoch_id = next_epoch_id++;
        epoch.op_indices.clear();
        break;
      case ThreadAPI::TD_MPI_WIN_UNLOCK:
        if (epoch.model == SyncModel::LOCK_UNLOCK && epoch.start) {
          for (size_t idx : epoch.op_indices) {
            rma_operations_[idx].sync_model = SyncModel::LOCK_UNLOCK;
            rma_operations_[idx].sync_start = epoch.start;
            rma_operations_[idx].sync_end = op.inst;
            rma_operations_[idx].epoch_id = epoch.epoch_id;
            rma_operations_[idx].synchronization_proof =
                concurrency::ProofStrength::Must;
          }
        }
        epoch.model = SyncModel::NONE;
        epoch.start = nullptr;
        epoch.epoch_id = 0;
        epoch.op_indices.clear();
        break;
      case ThreadAPI::TD_MPI_WIN_FLUSH:
      case ThreadAPI::TD_MPI_WIN_SYNC:
        // Completion/refinement only. They do not start a synchronization
        // epoch.
        break;
      case ThreadAPI::TD_MPI_WIN_START:
        epoch.model = SyncModel::PSCW;
        epoch.start = op.inst;
        epoch.epoch_id = next_epoch_id++;
        epoch.op_indices.clear();
        break;
      case ThreadAPI::TD_MPI_WIN_COMPLETE:
        if (epoch.model == SyncModel::PSCW && epoch.start) {
          for (size_t idx : epoch.op_indices) {
            rma_operations_[idx].sync_model = SyncModel::PSCW;
            rma_operations_[idx].sync_start = epoch.start;
            rma_operations_[idx].sync_end = op.inst;
            rma_operations_[idx].epoch_id = epoch.epoch_id;
            rma_operations_[idx].synchronization_proof =
                concurrency::ProofStrength::Must;
          }
        }
        epoch.model = SyncModel::NONE;
        epoch.start = nullptr;
        epoch.epoch_id = 0;
        epoch.op_indices.clear();
        break;
      case ThreadAPI::TD_MPI_WIN_POST:
      case ThreadAPI::TD_MPI_WIN_WAIT:
      case ThreadAPI::TD_MPI_WIN_TEST:
        // Exposure-side PSCW synchronization does not itself open a local
        // access epoch, but it should not discard an already-open access epoch.
        break;
      default:
        break;
      }
    }
  }
}

MPIRMAAnalysis::SyncModel
MPIRMAAnalysis::determineSyncModel(const RMAOperation &op) const {
  return op.sync_model;
}

bool MPIRMAAnalysis::areRMAOpsConflicting(const RMAOperation &op1,
                                          const RMAOperation &op2) const {
  // Same window
  if (op1.window != op2.window)
    return false;
  if (!rangesOverlap(op1.target_rank, op1.target_rank, op2.target_rank,
                     op2.target_rank) &&
      !rangesOverlap(op1.target_rank_min, op1.target_rank_max,
                     op2.target_rank_min, op2.target_rank_max)) {
    return false;
  }
  if (op1.target_disp != -1 && op2.target_disp != -1) {
    int64_t len1 = op1.byte_length > 0 ? op1.byte_length : 1;
    int64_t len2 = op2.byte_length > 0 ? op2.byte_length : 1;
    int64_t end1 = op1.target_disp + len1;
    int64_t end2 = op2.target_disp + len2;
    if (!(op1.target_disp < end2 && op2.target_disp < end1)) {
      return false;
    }
  }

  // At least one is a write (put or accumulate)
  const CallBase *CB1 = dyn_cast<CallBase>(op1.inst);
  const CallBase *CB2 = dyn_cast<CallBase>(op2.inst);
  if (!CB1 || !CB2)
    return false;

  const Function *F1 = CB1->getCalledFunction();
  const Function *F2 = CB2->getCalledFunction();
  if (!F1 || !F2)
    return false;

  ThreadAPI::TD_TYPE t1 = thread_api_->getType(F1);
  ThreadAPI::TD_TYPE t2 = thread_api_->getType(F2);
  bool op1_is_write =
      (t1 == ThreadAPI::TD_MPI_PUT || t1 == ThreadAPI::TD_MPI_ACCUMULATE);
  bool op2_is_write =
      (t2 == ThreadAPI::TD_MPI_PUT || t2 == ThreadAPI::TD_MPI_ACCUMULATE);

  if (!op1_is_write && !op2_is_write)
    return false; // Both reads

  // Different sync models or no sync
  if (op1.sync_model == SyncModel::NONE || op2.sync_model == SyncModel::NONE) {
    return true;
  }
  if (op1.sync_model != op2.sync_model) {
    return true; // Mixing sync models is problematic
  }
  if (op1.epoch_id != 0 && op1.epoch_id == op2.epoch_id) {
    return false;
  }
  // Two different ranks/process contexts using the same synchronization regime
  // can still race on the same location. Without a stronger cross-process
  // proof, keep the race report.
  return true;
}

std::vector<MPIRMAAnalysis::RMAOperation>
MPIRMAAnalysis::findUnsynchronizedRMAOps() const {
  std::vector<RMAOperation> unsync;
  for (const RMAOperation &op : rma_operations_) {
    if (op.sync_model == SyncModel::NONE) {
      unsync.push_back(op);
    }
  }
  return unsync;
}

std::vector<
    std::pair<MPIRMAAnalysis::RMAOperation, MPIRMAAnalysis::RMAOperation>>
MPIRMAAnalysis::findRMARaces() const {
  std::vector<std::pair<RMAOperation, RMAOperation>> races;

  for (size_t i = 0; i < rma_operations_.size(); ++i) {
    for (size_t j = i + 1; j < rma_operations_.size(); ++j) {
      if (areRMAOpsConflicting(rma_operations_[i], rma_operations_[j])) {
        races.emplace_back(rma_operations_[i], rma_operations_[j]);
      }
    }
  }

  return races;
}

std::vector<WindowID> MPIRMAAnalysis::findLeakedWindows() const {
  std::vector<WindowID> leaked;
  for (const auto &pair : windows_) {
    if (!pair.second.free_inst) {
      leaked.push_back(pair.first);
    }
  }
  return leaked;
}

// ============================================================================
// MPIAnalysis Implementation
// ============================================================================

void MPIAnalysis::runAnalysis() {
  // Run process model analysis
  process_model_.analyzeModule();

  // Run collective analysis
  collective_analysis_.analyzeCollectives();

  // Run RMA analysis
  rma_analysis_.analyzeRMA();

  // Collect results
  results_.orphaned_requests = process_model_.findOrphanedNonBlockingOps();
  results_.potential_deadlocks = process_model_.findPotentialDeadlocks();
  results_.mismatched_collectives =
      collective_analysis_.findMismatchedCollectives();
  results_.conditional_collectives =
      collective_analysis_.findConditionalCollectives();
  results_.unsynchronized_rma = rma_analysis_.findUnsynchronizedRMAOps();
  results_.rma_races = rma_analysis_.findRMARaces();
  results_.leaked_windows = rma_analysis_.findLeakedWindows();
}

void MPIAnalysis::printResults(raw_ostream &OS) const {
  const auto &operations = process_model_.getAllOperations();
  const auto &deferred = process_model_.getDeferredLoweringStats();

  auto countRequestStates = [&](RequestCompletionState state) {
    auto requestStatePriority = [](RequestCompletionState value) {
      switch (value) {
      case RequestCompletionState::Pending:
        return 0;
      case RequestCompletionState::MayComplete:
        return 1;
      case RequestCompletionState::MustComplete:
        return 2;
      case RequestCompletionState::Terminal:
        return 3;
      }
      return 0;
    };

    std::unordered_map<RequestID, RequestCompletionState> request_states;
    for (const auto &op : operations) {
      if (op.kind != MPIOpKind::SEND_NONBLOCKING &&
          op.kind != MPIOpKind::RECV_NONBLOCKING &&
          op.kind != MPIOpKind::BARRIER_NONBLOCKING &&
          op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
        continue;
      }
      if (!op.request) {
        continue;
      }
      auto it = request_states.find(op.request);
      if (it == request_states.end() || requestStatePriority(op.request_state) >
                                            requestStatePriority(it->second)) {
        request_states[op.request] = op.request_state;
      }
    }

    size_t count = 0;
    for (const auto &entry : request_states) {
      if (entry.second == state) {
        ++count;
      }
    }
    return count;
  };

  size_t deferred_total = 0;
  for (const auto &entry : deferred) {
    deferred_total += entry.second;
  }

  OS << "========================================\n";
  OS << "MPI Analysis Results\n";
  OS << "========================================\n\n";

  OS << "Total MPI operations found: " << operations.size() << "\n";
  OS << "MPI init/finalize ops: " << getOperationCount(MPIOpKind::INIT) << "/"
     << getOperationCount(MPIOpKind::FINALIZE) << "\n";
  OS << "Blocking point-to-point ops: "
     << getOperationCount(MPIOpKind::SEND_BLOCKING) +
            getOperationCount(MPIOpKind::RECV_BLOCKING)
     << "\n";
  OS << "Non-blocking MPI operations: "
     << getOperationCount(MPIOpKind::SEND_NONBLOCKING) +
            getOperationCount(MPIOpKind::RECV_NONBLOCKING) +
            getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "  Non-blocking point-to-point ops: "
     << getOperationCount(MPIOpKind::SEND_NONBLOCKING) +
            getOperationCount(MPIOpKind::RECV_NONBLOCKING)
     << "\n";
  OS << "  Probe ops (blocking/non-blocking): "
     << getOperationCount(MPIOpKind::PROBE_BLOCKING) << "/"
     << getOperationCount(MPIOpKind::PROBE_NONBLOCKING) << "\n";
  OS << "  Wait/Test ops: " << getOperationCount(MPIOpKind::WAIT) << "/"
     << getOperationCount(MPIOpKind::TEST) << "\n";
  OS << "Collective/barrier operations: "
     << getOperationCount(MPIOpKind::BARRIER_BLOCKING) +
            getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "  Blocking collective/barrier ops: "
     << getOperationCount(MPIOpKind::BARRIER_BLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING)
     << "\n";
  OS << "  Non-blocking collective/barrier ops: "
     << getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "Communicator management ops: "
     << getOperationCount(MPIOpKind::COMM_MANAGEMENT) << "\n";
  OS << "Request management ops: "
     << getOperationCount(MPIOpKind::REQUEST_MANAGEMENT) << "\n";
  OS << "RMA window lifecycle ops: " << getOperationCount(MPIOpKind::RMA_WINDOW)
     << "\n";
  OS << "RMA data ops: " << getOperationCount(MPIOpKind::RMA_DATA) << "\n";
  OS << "RMA sync ops: " << getOperationCount(MPIOpKind::RMA_SYNC) << "\n";
  OS << "Collective protocol slots tracked: "
     << getProtocolDiagnosticCount("collective_slots_tracked") << "\n";
  OS << "Collective partial-reachability observations: "
     << getProtocolDiagnosticCount("collective_partial_reachability") << "\n";
  OS << "Requests with may-complete status: "
     << countRequestStates(RequestCompletionState::MayComplete) << "\n";
  OS << "Requests with terminal status: "
     << countRequestStates(RequestCompletionState::Terminal) << "\n";
  OS << "Deferred MPI semantic lowering total: " << deferred_total << "\n\n";

  // Orphaned requests
  OS << "Orphaned non-blocking operations: "
     << results_.orphaned_requests.size() << "\n";
  for (const auto &req : results_.orphaned_requests) {
    OS << "  ";
    req.issue_inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  // Potential deadlocks
  OS << "Potential deadlocks: " << results_.potential_deadlocks.size() << "\n";
  for (const auto &pair : results_.potential_deadlocks) {
    OS << "  Send: ";
    pair.first->print(OS);
    OS << "\n  Recv: ";
    pair.second->print(OS);
    OS << "\n\n";
  }

  // Mismatched collectives
  OS << "Mismatched collectives: " << results_.mismatched_collectives.size()
     << "\n";
  for (const auto &pair : results_.mismatched_collectives) {
    OS << "  Collective 1: ";
    pair.first.inst->print(OS);
    OS << "\n  Collective 2: ";
    pair.second.inst->print(OS);
    OS << "\n\n";
  }

  // Conditional collectives
  OS << "Conditional collectives (may not be called by all processes): "
     << results_.conditional_collectives.size() << "\n";
  for (const auto *inst : results_.conditional_collectives) {
    OS << "  ";
    inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  // Unsynchronized RMA
  OS << "Unsynchronized RMA operations: " << results_.unsynchronized_rma.size()
     << "\n";
  for (const auto &op : results_.unsynchronized_rma) {
    OS << "  ";
    op.inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  // RMA races
  OS << "Potential RMA data races: " << results_.rma_races.size() << "\n";
  for (const auto &pair : results_.rma_races) {
    OS << "  Op 1: ";
    pair.first.inst->print(OS);
    OS << "\n  Op 2: ";
    pair.second.inst->print(OS);
    OS << "\n\n";
  }

  // Leaked windows
  OS << "Tracked RMA windows: " << getTrackedWindowCount() << "\n";
  OS << "Leaked RMA windows: " << results_.leaked_windows.size() << "\n\n";

  if (!deferred.empty()) {
    OS << "Deferred MPI semantic lowering:\n";
    for (const auto &entry : deferred) {
      OS << "  " << entry.first << ": " << entry.second << "\n";
    }
    OS << "\n";
  }

  OS << "========================================\n";
}

} // namespace mpi
