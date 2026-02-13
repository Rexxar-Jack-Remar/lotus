/**
 * @file MPIAnalysis.cpp
 * @brief MPI Program Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPIAnalysis.h"
#include "Analysis/Concurrency/Utils/LanguageModel/MPI.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace mpi {

// ============================================================================
// MPIProcessModel Implementation
// ============================================================================

MPIOpKind MPIProcessModel::classifyOperation(ThreadAPI::TD_TYPE type) const {
  switch (type) {
    case ThreadAPI::TD_MPI_INIT:
      return MPIOpKind::INIT;
    case ThreadAPI::TD_MPI_FINALIZE:
      return MPIOpKind::FINALIZE;
    case ThreadAPI::TD_MPI_SEND:
      return MPIOpKind::SEND_BLOCKING;
    case ThreadAPI::TD_MPI_RECV:
    case ThreadAPI::TD_MPI_PROBE:
    case ThreadAPI::TD_MPI_SENDRECV:
      return MPIOpKind::RECV_BLOCKING;
    case ThreadAPI::TD_MPI_ISEND:
      return MPIOpKind::SEND_NONBLOCKING;
    case ThreadAPI::TD_MPI_IRECV:
    case ThreadAPI::TD_MPI_IPROBE:
      return MPIOpKind::RECV_NONBLOCKING;
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
      return MPIOpKind::BARRIER;
    case ThreadAPI::TD_MPI_BCAST:
    case ThreadAPI::TD_MPI_SCATTER:
    case ThreadAPI::TD_MPI_GATHER:
    case ThreadAPI::TD_MPI_ALLGATHER:
    case ThreadAPI::TD_MPI_ALLTOALL:
    case ThreadAPI::TD_MPI_REDUCE:
    case ThreadAPI::TD_MPI_ALLREDUCE:
    case ThreadAPI::TD_MPI_REDUCE_SCATTER:
    case ThreadAPI::TD_MPI_SCAN:
      return MPIOpKind::COLLECTIVE;
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
    default:
      return MPIOpKind::UNKNOWN;
  }
}

void MPIProcessModel::extractOperationDetails(MPIOperation& op) {
  const CallBase* CB = dyn_cast<CallBase>(op.inst);
  if (!CB) return;

  // Extract communicator (typically last or second-to-last argument for collectives)
  // Extract rank information (dest/source - typically argument 0 for p2p)
  // Extract tag (typically argument 1 for p2p)
  // This is a simplified version - real implementation would need more sophisticated
  // constant propagation or symbolic analysis
  
  unsigned numArgs = CB->arg_size();
  if (numArgs == 0) return;

  // For point-to-point operations, try to extract rank and tag
  if (op.kind == MPIOpKind::SEND_BLOCKING || op.kind == MPIOpKind::SEND_NONBLOCKING) {
    // MPI_Send(buf, count, datatype, dest, tag, comm)
    if (numArgs >= 6) {
      if (const ConstantInt* dest = dyn_cast<ConstantInt>(CB->getArgOperand(3))) {
        op.dest_rank = dest->getSExtValue();
      }
      if (const ConstantInt* tag = dyn_cast<ConstantInt>(CB->getArgOperand(4))) {
        op.tag = tag->getSExtValue();
      }
      op.communicator = CB->getArgOperand(5);
    }
  } else if (op.kind == MPIOpKind::RECV_BLOCKING || op.kind == MPIOpKind::RECV_NONBLOCKING) {
    // MPI_Recv(buf, count, datatype, source, tag, comm, status)
    if (numArgs >= 7) {
      if (const ConstantInt* source = dyn_cast<ConstantInt>(CB->getArgOperand(3))) {
        op.source_rank = source->getSExtValue();
      }
      if (const ConstantInt* tag = dyn_cast<ConstantInt>(CB->getArgOperand(4))) {
        op.tag = tag->getSExtValue();
      }
      op.communicator = CB->getArgOperand(5);
    }
  }
  
  // For non-blocking operations, extract request handle
  if (op.kind == MPIOpKind::SEND_NONBLOCKING || op.kind == MPIOpKind::RECV_NONBLOCKING) {
    // Request is typically the last argument
    if (numArgs > 0) {
      op.request = CB->getArgOperand(numArgs - 1);
    }
  }
  
  // For wait operations, extract request
  if (op.kind == MPIOpKind::WAIT || op.kind == MPIOpKind::TEST) {
    // MPI_Wait(request, status)
    if (numArgs >= 1) {
      op.request = CB->getArgOperand(0);
    }
  }
  
  // For RMA operations, extract window and target rank
  if (op.kind == MPIOpKind::RMA_DATA) {
    // MPI_Put(origin_addr, origin_count, origin_datatype, target_rank, target_disp, 
    //         target_count, target_datatype, win)
    if (numArgs >= 8) {
      if (const ConstantInt* target = dyn_cast<ConstantInt>(CB->getArgOperand(3))) {
        op.target_rank = target->getSExtValue();
      }
      op.window = CB->getArgOperand(7);
    }
  }
}

void MPIProcessModel::analyzeModule() {
  all_operations_.clear();
  non_blocking_ops_.clear();

  // Iterate through all instructions in the module
  for (Function& F : module_) {
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      Instruction* I = &*II;
      
      const Function* callee = thread_api_->getCallee(I);
      if (!callee) continue;
      
      ThreadAPI::TD_TYPE type = thread_api_->getType(callee);
      if (type == ThreadAPI::TD_DUMMY) continue;
      
      // Check if this is an MPI operation
      MPIOpKind kind = classifyOperation(type);
      if (kind == MPIOpKind::UNKNOWN) continue;
      
      // Create operation record
      MPIOperation op(I, kind, type);
      extractOperationDetails(op);
      
      all_operations_.push_back(op);
      
      // Track non-blocking operations
      if (kind == MPIOpKind::SEND_NONBLOCKING || kind == MPIOpKind::RECV_NONBLOCKING) {
        if (op.request) {
          NonBlockingOp nbOp;
          nbOp.issue_inst = I;
          nbOp.request = op.request;
          nbOp.peer_rank = (kind == MPIOpKind::SEND_NONBLOCKING) ? op.dest_rank : op.source_rank;
          nbOp.tag = op.tag;
          nbOp.comm = op.communicator;
          non_blocking_ops_[op.request] = nbOp;
        }
      }
    }
  }
  
  // Match non-blocking operations with their completions
  matchNonBlockingOps();
}

void MPIProcessModel::matchNonBlockingOps() {
  for (const MPIOperation& op : all_operations_) {
    if (op.kind != MPIOpKind::WAIT && op.kind != MPIOpKind::TEST) continue;
    if (!op.request) continue;
    
    // Find the corresponding non-blocking operation
    auto it = non_blocking_ops_.find(op.request);
    if (it != non_blocking_ops_.end()) {
      it->second.is_completed = true;
      it->second.wait_inst = op.inst;
    }
  }
}

std::vector<MPIOperation> 
MPIProcessModel::getOperationsByKind(MPIOpKind kind) const {
  std::vector<MPIOperation> result;
  for (const MPIOperation& op : all_operations_) {
    if (op.kind == kind) {
      result.push_back(op);
    }
  }
  return result;
}

bool MPIProcessModel::canCommunicate(const MPIOperation& op1, 
                                     const MPIOperation& op2) const {
  // One must be send, other must be recv
  bool op1_is_send = (op1.kind == MPIOpKind::SEND_BLOCKING || 
                      op1.kind == MPIOpKind::SEND_NONBLOCKING);
  bool op2_is_send = (op2.kind == MPIOpKind::SEND_BLOCKING || 
                      op2.kind == MPIOpKind::SEND_NONBLOCKING);
  
  if (op1_is_send == op2_is_send) return false; // Both send or both recv
  
  const MPIOperation& send = op1_is_send ? op1 : op2;
  const MPIOperation& recv = op1_is_send ? op2 : op1;
  
  // Check if ranks match (send.dest == recv.source process)
  // This is simplified - real analysis would need inter-procedural rank tracking
  
  // Check if tags match
  if (send.tag != -1 && recv.tag != -1 && send.tag != recv.tag) {
    return false;
  }
  
  // Check if communicators match
  if (send.communicator && recv.communicator && 
      send.communicator != recv.communicator) {
    return false;
  }
  
  return true;
}

std::vector<MPIProcessModel::NonBlockingOp> 
MPIProcessModel::findOrphanedNonBlockingOps() const {
  std::vector<NonBlockingOp> orphaned;
  for (const auto& pair : non_blocking_ops_) {
    if (!pair.second.is_completed) {
      orphaned.push_back(pair.second);
    }
  }
  return orphaned;
}

std::vector<std::pair<const Instruction*, const Instruction*>>
MPIProcessModel::findPotentialDeadlocks() const {
  std::vector<std::pair<const Instruction*, const Instruction*>> deadlocks;
  
  // Simple deadlock pattern: all processes do blocking send before blocking recv
  // In the same function, if we see send before recv, that's a potential deadlock
  
  std::map<const Function*, std::vector<const Instruction*>> sends_by_func;
  std::map<const Function*, std::vector<const Instruction*>> recvs_by_func;
  
  for (const MPIOperation& op : all_operations_) {
    if (op.kind == MPIOpKind::SEND_BLOCKING) {
      sends_by_func[op.function].push_back(op.inst);
    } else if (op.kind == MPIOpKind::RECV_BLOCKING) {
      recvs_by_func[op.function].push_back(op.inst);
    }
  }
  
  // Check for send-before-recv pattern in each function
  for (const auto& pair : sends_by_func) {
    const Function* F = pair.first;
    const auto& sends = pair.second;
    const auto& recvs = recvs_by_func[F];
    
    for (const Instruction* send : sends) {
      for (const Instruction* recv : recvs) {
        // Simple dominance check (in same block, send comes before recv)
        if (send->getParent() == recv->getParent()) {
          bool send_before_recv = false;
          for (const Instruction& I : *send->getParent()) {
            if (&I == send) {
              send_before_recv = true;
              break;
            }
            if (&I == recv) break;
          }
          if (send_before_recv) {
            deadlocks.emplace_back(send, recv);
          }
        }
      }
    }
  }
  
  return deadlocks;
}

// ============================================================================
// MPICollectiveAnalysis Implementation
// ============================================================================

void MPICollectiveAnalysis::analyzeCollectives() {
  collective_calls_.clear();
  
  for (const MPIOperation& op : process_model_.getAllOperations()) {
    if (op.kind == MPIOpKind::COLLECTIVE || op.kind == MPIOpKind::BARRIER) {
      CollectiveCall call;
      call.inst = op.inst;
      call.type = op.td_type;
      call.comm = op.communicator;
      call.function = op.function;
      
      // Extract root rank for operations that need it
      if (op.td_type == ThreadAPI::TD_MPI_BCAST || 
          op.td_type == ThreadAPI::TD_MPI_REDUCE ||
          op.td_type == ThreadAPI::TD_MPI_GATHER ||
          op.td_type == ThreadAPI::TD_MPI_SCATTER) {
        const CallBase* CB = dyn_cast<CallBase>(op.inst);
        if (CB && CB->arg_size() > 5) {
          if (const ConstantInt* root = dyn_cast<ConstantInt>(CB->getArgOperand(5))) {
            call.root_rank = root->getSExtValue();
          }
        }
      }
      
      collective_calls_.push_back(call);
    }
  }
}

bool MPICollectiveAnalysis::areCollectivesCompatible(
    const CollectiveCall& c1, const CollectiveCall& c2) const {
  // Same collective type
  if (c1.type != c2.type) return false;
  
  // Same communicator
  if (c1.comm && c2.comm && c1.comm != c2.comm) return false;
  
  // For rooted collectives, check root
  if (c1.root_rank != -1 && c2.root_rank != -1 && c1.root_rank != c2.root_rank) {
    return false;
  }
  
  return true;
}

std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall, 
                      MPICollectiveAnalysis::CollectiveCall>>
MPICollectiveAnalysis::findMismatchedCollectives() const {
  std::vector<std::pair<CollectiveCall, CollectiveCall>> mismatches;
  
  // Compare all pairs of collective calls
  for (size_t i = 0; i < collective_calls_.size(); ++i) {
    for (size_t j = i + 1; j < collective_calls_.size(); ++j) {
      const CollectiveCall& c1 = collective_calls_[i];
      const CollectiveCall& c2 = collective_calls_[j];
      
      // If they're in the same function at different program points,
      // they might be called by different processes differently
      if (c1.function == c2.function && !areCollectivesCompatible(c1, c2)) {
        mismatches.emplace_back(c1, c2);
      }
    }
  }
  
  return mismatches;
}

std::vector<const Instruction*> 
MPICollectiveAnalysis::findConditionalCollectives() const {
  std::vector<const Instruction*> conditional;
  
  for (const CollectiveCall& call : collective_calls_) {
    const BasicBlock* BB = call.inst->getParent();
    
    // Simple heuristic: if the block has multiple predecessors,
    // the collective might be conditional
    if (pred_begin(BB) != pred_end(BB)) {
      unsigned pred_count = 0;
      for (auto it = pred_begin(BB); it != pred_end(BB); ++it) {
        ++pred_count;
      }
      if (pred_count > 1) {
        conditional.push_back(call.inst);
      }
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
  for (const MPIOperation& op : process_model_.getAllOperations()) {
    if (op.td_type == ThreadAPI::TD_MPI_WIN_CREATE) {
      RMAWindow window;
      window.window = nullptr; // Would need to extract from call
      window.create_inst = op.inst;
      
      // Extract window handle (typically last argument)
      const CallBase* CB = dyn_cast<CallBase>(op.inst);
      if (CB && CB->arg_size() > 0) {
        window.window = CB->getArgOperand(CB->arg_size() - 1);
        windows_[window.window] = window;
      }
    } else if (op.td_type == ThreadAPI::TD_MPI_WIN_FREE) {
      const CallBase* CB = dyn_cast<CallBase>(op.inst);
      if (CB && CB->arg_size() > 0) {
        WindowID win = CB->getArgOperand(0);
        auto it = windows_.find(win);
        if (it != windows_.end()) {
          it->second.free_inst = op.inst;
        }
      }
    }
  }
  
  // Second pass: collect RMA operations and sync
  for (const MPIOperation& op : process_model_.getAllOperations()) {
    if (op.kind == MPIOpKind::RMA_DATA) {
      RMAOperation rma_op;
      rma_op.inst = op.inst;
      rma_op.window = op.window;
      rma_op.target_rank = op.target_rank;
      rma_op.sync_model = determineSyncModel(rma_op);
      
      rma_operations_.push_back(rma_op);
      
      // Add to window tracking
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
    }
  }
}

MPIRMAAnalysis::SyncModel 
MPIRMAAnalysis::determineSyncModel(const RMAOperation& op) const {
  auto it = windows_.find(op.window);
  if (it == windows_.end()) return SyncModel::NONE;
  
  const RMAWindow& window = it->second;
  
  // Simple heuristic: check what sync operations exist for this window
  if (!window.fence_ops.empty()) return SyncModel::FENCE;
  if (!window.lock_ops.empty()) return SyncModel::LOCK_UNLOCK;
  
  // Would need more sophisticated control flow analysis for PSCW
  
  return SyncModel::NONE;
}

bool MPIRMAAnalysis::areRMAOpsConflicting(const RMAOperation& op1, 
                                         const RMAOperation& op2) const {
  // Same window
  if (op1.window != op2.window) return false;
  
  // At least one is a write (put or accumulate)
  const CallBase* CB1 = dyn_cast<CallBase>(op1.inst);
  const CallBase* CB2 = dyn_cast<CallBase>(op2.inst);
  if (!CB1 || !CB2) return false;
  
  Function* F1 = CB1->getCalledFunction();
  Function* F2 = CB2->getCalledFunction();
  if (!F1 || !F2) return false;
  
  bool op1_is_write = (MPIModel::isPut(F1->getName()) || 
                       MPIModel::isAccumulate(F1->getName()));
  bool op2_is_write = (MPIModel::isPut(F2->getName()) || 
                       MPIModel::isAccumulate(F2->getName()));
  
  if (!op1_is_write && !op2_is_write) return false; // Both reads
  
  // Different sync models or no sync
  if (op1.sync_model == SyncModel::NONE || op2.sync_model == SyncModel::NONE) {
    return true;
  }
  if (op1.sync_model != op2.sync_model) {
    return true; // Mixing sync models is problematic
  }
  
  return false;
}

std::vector<MPIRMAAnalysis::RMAOperation> 
MPIRMAAnalysis::findUnsynchronizedRMAOps() const {
  std::vector<RMAOperation> unsync;
  for (const RMAOperation& op : rma_operations_) {
    if (op.sync_model == SyncModel::NONE) {
      unsync.push_back(op);
    }
  }
  return unsync;
}

std::vector<std::pair<MPIRMAAnalysis::RMAOperation, MPIRMAAnalysis::RMAOperation>>
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
  for (const auto& pair : windows_) {
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
  results_.mismatched_collectives = collective_analysis_.findMismatchedCollectives();
  results_.conditional_collectives = collective_analysis_.findConditionalCollectives();
  results_.unsynchronized_rma = rma_analysis_.findUnsynchronizedRMAOps();
  results_.rma_races = rma_analysis_.findRMARaces();
  results_.leaked_windows = rma_analysis_.findLeakedWindows();
}

void MPIAnalysis::printResults(raw_ostream& OS) const {
  OS << "========================================\n";
  OS << "MPI Analysis Results\n";
  OS << "========================================\n\n";
  
  OS << "Total MPI operations found: " << process_model_.getAllOperations().size() << "\n\n";
  
  // Orphaned requests
  OS << "Orphaned non-blocking operations: " << results_.orphaned_requests.size() << "\n";
  for (const auto& req : results_.orphaned_requests) {
    OS << "  ";
    req.issue_inst->print(OS);
    OS << "\n";
  }
  OS << "\n";
  
  // Potential deadlocks
  OS << "Potential deadlocks: " << results_.potential_deadlocks.size() << "\n";
  for (const auto& pair : results_.potential_deadlocks) {
    OS << "  Send: ";
    pair.first->print(OS);
    OS << "\n  Recv: ";
    pair.second->print(OS);
    OS << "\n\n";
  }
  
  // Mismatched collectives
  OS << "Mismatched collectives: " << results_.mismatched_collectives.size() << "\n";
  for (const auto& pair : results_.mismatched_collectives) {
    OS << "  Collective 1: ";
    pair.first.inst->print(OS);
    OS << "\n  Collective 2: ";
    pair.second.inst->print(OS);
    OS << "\n\n";
  }
  
  // Conditional collectives
  OS << "Conditional collectives (may not be called by all processes): " 
     << results_.conditional_collectives.size() << "\n";
  for (const auto* inst : results_.conditional_collectives) {
    OS << "  ";
    inst->print(OS);
    OS << "\n";
  }
  OS << "\n";
  
  // Unsynchronized RMA
  OS << "Unsynchronized RMA operations: " << results_.unsynchronized_rma.size() << "\n";
  for (const auto& op : results_.unsynchronized_rma) {
    OS << "  ";
    op.inst->print(OS);
    OS << "\n";
  }
  OS << "\n";
  
  // RMA races
  OS << "Potential RMA data races: " << results_.rma_races.size() << "\n";
  for (const auto& pair : results_.rma_races) {
    OS << "  Op 1: ";
    pair.first.inst->print(OS);
    OS << "\n  Op 2: ";
    pair.second.inst->print(OS);
    OS << "\n\n";
  }
  
  // Leaked windows
  OS << "Leaked RMA windows: " << results_.leaked_windows.size() << "\n\n";
  
  OS << "========================================\n";
}

} // namespace mpi
