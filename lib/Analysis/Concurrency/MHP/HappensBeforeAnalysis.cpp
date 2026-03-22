#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/OpenMP/OpenMPSemantics.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

namespace {

bool hasBranchWitness(const Instruction *inst) {
  if (!inst) {
    return false;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(inst);
  visited.insert(inst);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    for (const User *user : current->users()) {
      if (!visited.insert(user).second) {
        continue;
      }
      if (const auto *cmp = dyn_cast<ICmpInst>(user)) {
        for (const User *cmp_user : cmp->users()) {
          const auto *branch = dyn_cast<BranchInst>(cmp_user);
          if (branch && branch->isConditional()) {
            return true;
          }
        }
        continue;
      }
      if (isa<CastInst>(user) || isa<BinaryOperator>(user) ||
          isa<SelectInst>(user) || isa<PHINode>(user)) {
        worklist.push_back(user);
      }
    }
  }

  return false;
}

bool hasCmpXchgSuccessWitness(const Instruction *inst) {
  if (!isa<AtomicCmpXchgInst>(inst)) {
    return false;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(inst);
  visited.insert(inst);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    for (const User *user : current->users()) {
      if (!visited.insert(user).second) {
        continue;
      }
      if (const auto *extract = dyn_cast<ExtractValueInst>(user)) {
        if (extract->getNumIndices() == 1 && extract->getIndices()[0] == 1) {
          worklist.push_back(extract);
        }
        continue;
      }
      if (const auto *branch = dyn_cast<BranchInst>(user)) {
        if (branch->isConditional() && branch->getCondition() == current) {
          return true;
        }
        continue;
      }
      if (const auto *cmp = dyn_cast<ICmpInst>(user)) {
        for (const User *cmp_user : cmp->users()) {
          const auto *branch = dyn_cast<BranchInst>(cmp_user);
          if (branch && branch->isConditional()) {
            return true;
          }
        }
        continue;
      }
      if (isa<CastInst>(user) || isa<SelectInst>(user) || isa<PHINode>(user)) {
        worklist.push_back(user);
      }
    }
  }

  return false;
}

const BranchInst *getBranchWitness(const Instruction *inst) {
  if (!inst) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(inst);
  visited.insert(inst);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    for (const User *user : current->users()) {
      if (!visited.insert(user).second) {
        continue;
      }
      if (const auto *cmp = dyn_cast<ICmpInst>(user)) {
        for (const User *cmp_user : cmp->users()) {
          const auto *branch = dyn_cast<BranchInst>(cmp_user);
          if (branch && branch->isConditional()) {
            return branch;
          }
        }
        continue;
      }
      if (isa<CastInst>(user) || isa<BinaryOperator>(user) ||
          isa<SelectInst>(user) || isa<PHINode>(user)) {
        worklist.push_back(user);
      }
    }
  }

  return nullptr;
}

const BasicBlock *getWitnessSuccessor(const Instruction *inst) {
  const auto *branch = getBranchWitness(inst);
  if (!branch) {
    return nullptr;
  }

  const auto *cmp = dyn_cast<ICmpInst>(branch->getCondition());
  if (!cmp) {
    return nullptr;
  }

  if (cmp->getPredicate() != ICmpInst::ICMP_EQ &&
      cmp->getPredicate() != ICmpInst::ICMP_NE) {
    return nullptr;
  }

  const Value *lhs = cmp->getOperand(0);
  const Value *rhs = cmp->getOperand(1);
  const auto *lhs_const = dyn_cast<ConstantInt>(lhs);
  const auto *rhs_const = dyn_cast<ConstantInt>(rhs);
  const ConstantInt *constant = lhs_const ? lhs_const : rhs_const;
  if (!constant) {
    return nullptr;
  }

  auto choose = [&](bool sync_on_true) -> const BasicBlock * {
    return branch->getSuccessor(sync_on_true ? 0 : 1);
  };

  const bool is_zero = constant->isZero();
  if (cmp->getPredicate() == ICmpInst::ICMP_NE) {
    return choose(is_zero);
  }
  return choose(!is_zero);
}

struct AtomicLocationKey {
  const Value *base = nullptr;
  int64_t offset = 0;
  bool has_precise_offset = false;

  bool operator==(const AtomicLocationKey &other) const {
    return base == other.base && offset == other.offset &&
           has_precise_offset == other.has_precise_offset;
  }
};

struct AtomicLocationKeyHash {
  size_t operator()(const AtomicLocationKey &key) const {
    size_t seed = std::hash<const Value *>{}(key.base);
    seed ^= std::hash<int64_t>{}(key.offset) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    seed ^= std::hash<bool>{}(key.has_precise_offset) + 0x9e3779b9 +
            (seed << 6) + (seed >> 2);
    return seed;
  }
};

AtomicLocationKey getExactAtomicLocation(const Instruction *inst) {
  AtomicLocationKey key;
  const Value *ptr = CppAtomics::getAtomicPointer(inst);
  if (!ptr) {
    return key;
  }

  ptr = ptr->stripPointerCasts();
  int64_t offset = 0;
  if (const Value *base =
          GetPointerBaseWithConstantOffset(ptr, offset,
                                           inst->getModule()->getDataLayout())) {
    key.base = base->stripPointerCasts();
    key.offset = offset;
    key.has_precise_offset = true;
    return key;
  }

  if (const Value *base = getUnderlyingObject(ptr)) {
    key.base = base->stripPointerCasts();
    return key;
  }
  key.base = ptr;
  return key;
}

} // namespace

HappensBeforeAnalysis::HappensBeforeAnalysis(Module &module,
                                             mhp::MHPAnalysis &mhp)
    : m_module(module), m_mhp(mhp), m_alias_analysis(mhp.getAliasAnalysis()) {}

void HappensBeforeAnalysis::analyze() {
  m_hb_cache.clear();
  m_future_shared_state.clear();
  m_shared_state_trace_cache.clear();
  m_deferred_sync_counts.clear();
  m_atomic_instructions.clear();
  m_sync_with.clear();
  m_explicit_hb_pairs.clear();
  m_extra_hb_successors.clear();
  m_post_dom_cache.clear();

  computeAtomicHappensBefore();
  buildSynchronizesWith();
}

void HappensBeforeAnalysis::addExtraHBEdge(const Instruction *from,
                                           const Instruction *to) {
  if (!from || !to || from == to) {
    return;
  }

  mhp::ThreadID from_tid = m_mhp.getThreadID(from);
  mhp::ThreadID to_tid = m_mhp.getThreadID(to);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::vector<mhp::SyncNode *> from_nodes =
      from_tid == std::numeric_limits<mhp::ThreadID>::max()
          ? tfg.getNodes(from)
          : tfg.getNodes(from, from_tid);
  std::vector<mhp::SyncNode *> to_nodes =
      to_tid == std::numeric_limits<mhp::ThreadID>::max()
          ? tfg.getNodes(to)
          : tfg.getNodes(to, to_tid);
  for (mhp::SyncNode *from_node : from_nodes) {
    auto &succs = m_extra_hb_successors[from_node];
    for (mhp::SyncNode *to_node : to_nodes) {
      if (std::find(succs.begin(), succs.end(), to_node) == succs.end()) {
        succs.push_back(to_node);
      }
    }
  }
}

void HappensBeforeAnalysis::addExplicitHBPair(const Instruction *from,
                                              const Instruction *to) {
  if (!from || !to || from == to) {
    return;
  }
  m_explicit_hb_pairs.insert({from, to});
}

std::vector<const Instruction *>
HappensBeforeAnalysis::collectThreadPrefixInstructions(
    const Instruction *inst) const {
  std::vector<const Instruction *> ordered;
  if (!inst || isInstructionThreadAmbiguous(inst)) {
    return ordered;
  }

  mhp::ThreadID tid = m_mhp.getThreadID(inst);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::deque<const mhp::SyncNode *> worklist;
  std::unordered_set<const mhp::SyncNode *> visited;
  for (mhp::SyncNode *node : tfg.getNodes(inst, tid)) {
    worklist.push_back(node);
    visited.insert(node);
  }

  std::set<const Instruction *> collected;
  while (!worklist.empty()) {
    const mhp::SyncNode *current = worklist.front();
    worklist.pop_front();
    if (const Instruction *current_inst = current->getInstruction()) {
      collected.insert(current_inst);
    }
    for (mhp::SyncNode *pred : current->getPredecessors()) {
      if (pred->getThreadID() != tid) {
        continue;
      }
      if (visited.insert(pred).second) {
        worklist.push_back(pred);
      }
    }
  }

  ordered.assign(collected.begin(), collected.end());
  return ordered;
}

std::vector<const Instruction *>
HappensBeforeAnalysis::collectThreadSuffixInstructions(
    const Instruction *inst) const {
  std::vector<const Instruction *> ordered;
  if (!inst || isInstructionThreadAmbiguous(inst)) {
    return ordered;
  }

  mhp::ThreadID tid = m_mhp.getThreadID(inst);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::deque<const mhp::SyncNode *> worklist;
  std::unordered_set<const mhp::SyncNode *> visited;
  for (mhp::SyncNode *node : tfg.getNodes(inst, tid)) {
    worklist.push_back(node);
    visited.insert(node);
  }

  std::set<const Instruction *> collected;
  while (!worklist.empty()) {
    const mhp::SyncNode *current = worklist.front();
    worklist.pop_front();
    if (const Instruction *current_inst = current->getInstruction()) {
      collected.insert(current_inst);
    }
    for (mhp::SyncNode *succ : current->getSuccessors()) {
      if (succ->getThreadID() != tid) {
        continue;
      }
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  ordered.assign(collected.begin(), collected.end());
  return ordered;
}

std::vector<const Instruction *>
HappensBeforeAnalysis::collectHBRelevantSuffixInstructions(
    const Instruction *inst) const {
  std::vector<const Instruction *> ordered;
  if (!inst || isInstructionThreadAmbiguous(inst)) {
    return ordered;
  }

  std::set<const Instruction *> collected;
  if (const BasicBlock *sync_successor = getWitnessSuccessor(inst)) {
    std::deque<const BasicBlock *> worklist;
    std::unordered_set<const BasicBlock *> visited;
    worklist.push_back(sync_successor);
    visited.insert(sync_successor);

    while (!worklist.empty()) {
      const BasicBlock *current = worklist.front();
      worklist.pop_front();
      for (const Instruction &candidate : *current) {
        collected.insert(&candidate);
      }
      for (const BasicBlock *succ : successors(current)) {
        if (visited.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
  } else {
    mhp::ThreadID tid = m_mhp.getThreadID(inst);
    const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
    for (const Instruction *candidate : collectThreadSuffixInstructions(inst)) {
      if (!candidate || candidate == inst) {
        continue;
      }
      if (!isPostSyncInstruction(inst, candidate)) {
        continue;
      }
      if (tfg.getNodes(candidate, tid).empty()) {
        continue;
      }
      collected.insert(candidate);
    }
  }

  ordered.assign(collected.begin(), collected.end());
  return ordered;
}

bool HappensBeforeAnalysis::isPostSyncInstruction(
    const Instruction *sync_inst, const Instruction *candidate) const {
  if (!sync_inst || !candidate || sync_inst == candidate) {
    return false;
  }
  if (sync_inst->getFunction() != candidate->getFunction()) {
    return true;
  }
  if (sync_inst->getParent() == candidate->getParent()) {
    bool seen_sync = false;
    for (const Instruction &inst : *sync_inst->getParent()) {
      if (&inst == sync_inst) {
        seen_sync = true;
        continue;
      }
      if (&inst == candidate) {
        return seen_sync;
      }
    }
    return false;
  }

  const llvm::PostDominatorTree &pdt =
      getPostDominatorTree(sync_inst->getFunction());
  return pdt.dominates(candidate->getParent(), sync_inst->getParent());
}

void HappensBeforeAnalysis::addExplicitHBClosure(const Instruction *from,
                                                 const Instruction *to) {
  for (const Instruction *prefix : collectThreadPrefixInstructions(from)) {
    addExplicitHBPair(prefix, to);
    for (const Instruction *suffix : collectHBRelevantSuffixInstructions(to)) {
      addExplicitHBPair(prefix, suffix);
    }
  }
}

void HappensBeforeAnalysis::computeAtomicHappensBefore() {
  errs() << "Computing Atomic Happens-Before...\n";

  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if (CppAtomics::isAtomic(&*I)) {
        m_atomic_instructions.push_back(&*I);
      }
    }
  }

  errs() << "Deferred " << m_atomic_instructions.size()
         << " atomic/fence operations for witness-aware lowering.\n";
}

void HappensBeforeAnalysis::buildSynchronizesWith() {
  using namespace CppAtomics;

  struct AtomicEvent {
    const Instruction *inst = nullptr;
    AtomicLocationKey location;
    mhp::ThreadID tid = std::numeric_limits<mhp::ThreadID>::max();
    bool is_store_like = false;
    bool is_load_like = false;
    bool is_rmw = false;
    bool is_cmpxchg = false;
    bool has_success_witness = true;
    bool has_release = false;
    bool has_acquire = false;
    bool is_fence = false;
  };

  struct AtomicTarget {
    const Instruction *anchor = nullptr;
    const Instruction *target = nullptr;
    bool is_fence_target = false;
  };

  std::vector<const Instruction *> release_ops;
  std::vector<const Instruction *> acquire_ops;
  std::vector<const Instruction *> promise_sets;
  std::vector<const Instruction *> future_gets;
  std::vector<const Instruction *> call_once_ops;
  std::vector<const Instruction *> latch_countdowns;
  std::vector<const Instruction *> latch_waits;
  std::vector<const Instruction *> barrier_arrives;
  std::vector<const Instruction *> barrier_waits;
  std::vector<const Instruction *> omp_task_ops;
  std::vector<AtomicEvent> atomic_events;
  std::unordered_map<AtomicLocationKey, std::vector<size_t>,
                     AtomicLocationKeyHash>
      events_by_location;
  std::unordered_map<const Instruction *, const Instruction *>
      release_fence_anchor;
  std::unordered_map<const Instruction *, const Instruction *>
      acquire_fence_anchor;

  std::set<std::pair<const Instruction *, const Instruction *>> seen_sync_edges;
  auto addSyncEdge = [&](const Instruction *from, const Instruction *to) {
    if (!from || !to || from == to) {
      return;
    }
    if (seen_sync_edges.emplace(from, to).second) {
      m_sync_with.emplace_back(from, to);
      addExplicitHBClosure(from, to);
    }
  };

  ThreadAPI *threadAPI = ThreadAPI::getThreadAPI();
  auto findCallOnceCallable = [&](const CallBase *call) -> const Function * {
    if (!call) {
      return nullptr;
    }
    for (unsigned idx = 1; idx < call->arg_size(); ++idx) {
      const Value *arg = call->getArgOperand(idx);
      if (!arg) {
        continue;
      }
      arg = arg->stripPointerCasts();
      if (const auto *func = dyn_cast<Function>(arg)) {
        return func->isDeclaration() ? nullptr : func;
      }
    }
    return nullptr;
  };
  auto getFunctionReturns = [](const Function *func) {
    std::vector<const Instruction *> exits;
    if (!func || func->isDeclaration()) {
      return exits;
    }
    for (const BasicBlock &bb : *func) {
      if (const auto *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
        exits.push_back(ret);
      }
    }
    return exits;
  };

  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;

      if (isAtomic(inst)) {
        AtomicEvent event;
        event.inst = inst;
        event.location = getExactAtomicLocation(inst);
        event.tid = m_mhp.getThreadID(inst);
        event.is_store_like = isStore(inst);
        event.is_load_like = isLoad(inst);
        event.is_rmw = isReadModifyWrite(inst);
        event.is_cmpxchg = isa<AtomicCmpXchgInst>(inst);
        event.has_success_witness =
            !event.is_cmpxchg || hasCmpXchgSuccessWitness(inst);
        event.has_release = hasReleaseSemantics(inst);
        event.has_acquire = hasAcquireSemantics(inst);
        event.is_fence = false;
        atomic_events.push_back(event);
        if (event.location.base) {
          events_by_location[event.location].push_back(atomic_events.size() - 1);
        }

        if (hasReleaseSemantics(inst) &&
            (isStore(inst) || isReadModifyWrite(inst))) {
          release_ops.push_back(inst);
        }
        if (hasAcquireSemantics(inst) &&
            (isLoad(inst) || isReadModifyWrite(inst))) {
          acquire_ops.push_back(inst);
        }
      }

      if (isFenceRelease(inst) || isFenceAcqRel(inst) || isFenceSeqCst(inst)) {
        release_ops.push_back(inst);
      }
      if (isFenceAcquire(inst) || isFenceAcqRel(inst) || isFenceSeqCst(inst)) {
        acquire_ops.push_back(inst);
      }

      const CallBase *call = dyn_cast<CallBase>(inst);
      if (!call) {
        continue;
      }

      const Function *callee = threadAPI->getCallee(call);
      ThreadAPI::TD_TYPE type = threadAPI->getType(call);

      if (callee && callee->getName().contains("get_future") &&
          call->arg_size() >= 1) {
        const Value *promise_obj = traceSharedState(call->getArgOperand(0));
        if (promise_obj) {
          m_future_shared_state[call] = promise_obj;
        }
      }

      switch (type) {
      case ThreadAPI::TD_PROMISE_SET:
        promise_sets.push_back(inst);
        break;
      case ThreadAPI::TD_FUTURE_GET:
      case ThreadAPI::TD_FUTURE_WAIT:
        future_gets.push_back(inst);
        break;
      case ThreadAPI::TD_CALL_ONCE:
        call_once_ops.push_back(inst);
        break;
      case ThreadAPI::TD_LATCH_COUNT_DOWN:
      case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
        latch_countdowns.push_back(inst);
        if (type == ThreadAPI::TD_LATCH_ARRIVE_WAIT) {
          latch_waits.push_back(inst);
        }
        break;
      case ThreadAPI::TD_LATCH_WAIT:
        latch_waits.push_back(inst);
        break;
      case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
        barrier_arrives.push_back(inst);
        barrier_waits.push_back(inst);
        break;
      case ThreadAPI::TD_BARRIER_ARRIVE:
        barrier_arrives.push_back(inst);
        break;
      case ThreadAPI::TD_BARRIER_WAIT_CPP20:
        barrier_waits.push_back(inst);
        break;
      case ThreadAPI::TD_OMP_TASK:
      case ThreadAPI::TD_OMP_TASKWAIT:
      case ThreadAPI::TD_OMP_TASKWAIT_DEPS:
      case ThreadAPI::TD_OMP_TASKYIELD:
      case ThreadAPI::TD_OMP_TASKGROUP_START:
      case ThreadAPI::TD_OMP_TASKGROUP_END:
      case ThreadAPI::TD_OMP_TASK_WITH_DEPS:
      case ThreadAPI::TD_OMP_TASKLOOP:
      case ThreadAPI::TD_OMP_TASK_COMPLETE:
      case ThreadAPI::TD_OMP_SINGLE_START:
      case ThreadAPI::TD_OMP_SINGLE_END:
      case ThreadAPI::TD_OMP_MASTER_START:
      case ThreadAPI::TD_OMP_MASTER_END:
      case ThreadAPI::TD_OMP_ORDERED_START:
      case ThreadAPI::TD_OMP_ORDERED_END:
      case ThreadAPI::TD_OMP_REDUCE_START:
      case ThreadAPI::TD_OMP_REDUCE_END:
      case ThreadAPI::TD_OMP_REDUCE_NOWAIT_START:
      case ThreadAPI::TD_OMP_REDUCE_NOWAIT_END:
      case ThreadAPI::TD_OMP_FOR_STATIC_INIT:
      case ThreadAPI::TD_OMP_FOR_STATIC_FINI:
      case ThreadAPI::TD_OMP_FOR_DISPATCH_INIT:
      case ThreadAPI::TD_OMP_FOR_DISPATCH_NEXT:
      case ThreadAPI::TD_OMP_FOR_DISPATCH_FINI:
      case ThreadAPI::TD_OMP_FLUSH:
        omp_task_ops.push_back(inst);
        break;
      default:
        break;
      }
    }
  }

  for (const Instruction *P : promise_sets) {
    for (const Instruction *F : future_gets) {
      if (samePromiseFuturePair(P, F)) {
        addSyncEdge(P, F);
      }
    }
  }

  size_t direct_atomic_edges = 0;
  size_t deferred_direct_atomic_relations = 0;
  size_t mixed_fence_atomic_edges = 0;
  size_t deferred_mixed_fence_relations = 0;
  size_t release_sequence_edges = 0;
  size_t deferred_release_sequence_relations = 0;
  for (const AtomicEvent &event : atomic_events) {
    if (!event.location.base || isInstructionThreadAmbiguous(event.inst)) {
      continue;
    }

    if (event.is_store_like) {
      const Instruction *cursor = event.inst->getPrevNode();
      while (cursor) {
        if (CppAtomics::isFence(cursor)) {
          if (CppAtomics::isFenceRelease(cursor) ||
              CppAtomics::isFenceAcqRel(cursor) ||
              CppAtomics::isFenceSeqCst(cursor)) {
            release_fence_anchor[cursor] = event.inst;
          }
          break;
        }
        if (!isFenceAnchorCompatibleInstruction(cursor)) {
          break;
        }
        cursor = cursor->getPrevNode();
      }
    }

    if (event.is_load_like) {
      const Instruction *cursor = event.inst->getNextNode();
      while (cursor) {
        if (CppAtomics::isFence(cursor)) {
          if (CppAtomics::isFenceAcquire(cursor) ||
              CppAtomics::isFenceAcqRel(cursor) ||
              CppAtomics::isFenceSeqCst(cursor)) {
            acquire_fence_anchor[cursor] = event.inst;
          }
          break;
        }
        if (!isFenceAnchorCompatibleInstruction(cursor)) {
          break;
        }
        cursor = cursor->getNextNode();
      }
    }
  }

  auto getReleaseCandidates = [&](const AtomicLocationKey &location) {
    std::vector<std::pair<const Instruction *, const Instruction *>> candidates;
    if (!location.base) {
      return candidates;
    }

    auto loc_it = events_by_location.find(location);
    if (loc_it == events_by_location.end() || loc_it->second.empty()) {
      return candidates;
    }
    const Instruction *representative = atomic_events[loc_it->second.front()].inst;

    const AtomicEvent *release_head = nullptr;
    std::vector<const AtomicEvent *> rmw_candidates;
    bool has_non_release_store_like = false;
    for (const auto &bucket : events_by_location) {
      if (bucket.second.empty()) {
        continue;
      }
      const Instruction *bucket_rep = atomic_events[bucket.second.front()].inst;
      if (representative != bucket_rep &&
          !sameAtomicLocation(representative, bucket_rep)) {
        continue;
      }
      for (size_t idx : bucket.second) {
        const AtomicEvent &event = atomic_events[idx];
        if (!event.is_store_like ||
            event.tid == std::numeric_limits<mhp::ThreadID>::max()) {
          continue;
        }
        if (!event.has_release) {
          has_non_release_store_like = true;
          continue;
        }
        if (event.is_cmpxchg && !event.has_success_witness) {
          ++deferred_direct_atomic_relations;
          ++m_deferred_sync_counts["atomic_cmpxchg_release_missing_success_witness"];
          continue;
        }
        if (event.is_rmw) {
          rmw_candidates.push_back(&event);
          continue;
        }
        if (release_head) {
          candidates.clear();
          return candidates;
        }
        release_head = &event;
      }
    }

    if (has_non_release_store_like &&
        (release_head != nullptr || !rmw_candidates.empty())) {
      ++m_deferred_sync_counts["atomic_nonrelease_store_competes_with_release"];
      candidates.clear();
      return candidates;
    }

    if (release_head) {
      for (const AtomicEvent *rmw_candidate : rmw_candidates) {
        if (hasProgramOrder(rmw_candidate->inst, release_head->inst)) {
          candidates.clear();
          return candidates;
        }
      }
    } else if (rmw_candidates.size() == 1) {
      candidates.emplace_back(rmw_candidates.front()->inst,
                              rmw_candidates.front()->inst);
    } else if (rmw_candidates.size() > 1) {
      candidates.clear();
      return candidates;
    }

    for (const auto &entry : release_fence_anchor) {
      if (!sameAtomicLocation(entry.second, representative)) {
        continue;
      }
      if (release_head || !candidates.empty()) {
        candidates.clear();
        return candidates;
      }
      candidates.emplace_back(entry.first, entry.second);
    }

    if (release_head) {
      candidates.emplace_back(release_head->inst, release_head->inst);
    }

    return candidates;
  };

  auto getAcquireTargets = [&](const AtomicLocationKey &location) {
    std::vector<AtomicTarget> targets;
    auto loc_it = events_by_location.find(location);
    if (loc_it == events_by_location.end() || loc_it->second.empty()) {
      return targets;
    }
    const Instruction *representative = atomic_events[loc_it->second.front()].inst;

    for (const auto &bucket : events_by_location) {
      if (bucket.second.empty()) {
        continue;
      }
      const Instruction *bucket_rep = atomic_events[bucket.second.front()].inst;
      if (representative != bucket_rep &&
          !sameAtomicLocation(representative, bucket_rep)) {
        continue;
      }
      for (size_t idx : bucket.second) {
        const AtomicEvent &event = atomic_events[idx];
        if (!event.is_load_like || !event.has_acquire ||
            event.tid == std::numeric_limits<mhp::ThreadID>::max()) {
          continue;
        }
        if (event.is_cmpxchg && !event.has_success_witness) {
          ++deferred_direct_atomic_relations;
          ++m_deferred_sync_counts["atomic_cmpxchg_acquire_missing_success_witness"];
          continue;
        }
        if (!event.is_rmw && !hasBranchWitness(event.inst)) {
          ++deferred_direct_atomic_relations;
          ++m_deferred_sync_counts["atomic_direct_missing_branch_witness"];
          continue;
        }
        targets.push_back({event.inst, event.inst, false});
      }
    }

    for (const auto &entry : acquire_fence_anchor) {
      if (!sameAtomicLocation(entry.second, representative)) {
        continue;
      }
      if (!hasBranchWitness(entry.second)) {
        ++deferred_mixed_fence_relations;
        ++m_deferred_sync_counts["atomic_mixed_fence_missing_branch_witness"];
        continue;
      }
      targets.push_back({entry.second, entry.second, true});
    }

    return targets;
  };

  for (const auto &entry : events_by_location) {
    const AtomicLocationKey &location = entry.first;
    auto release_candidates = getReleaseCandidates(location);
    if (release_candidates.size() != 1) {
      ++m_deferred_sync_counts["atomic_release_candidate_unresolved"];
      for (size_t idx : entry.second) {
        const AtomicEvent &event = atomic_events[idx];
        if (event.is_load_like && event.has_acquire) {
          ++deferred_direct_atomic_relations;
        }
      }
      continue;
    }

    bool has_release_sequence = false;
    for (size_t idx : entry.second) {
      const AtomicEvent &event = atomic_events[idx];
      if (event.inst != release_candidates.front().second && event.is_store_like &&
          event.is_rmw && (!event.is_cmpxchg || event.has_success_witness)) {
        has_release_sequence = true;
        break;
      }
    }

    std::vector<AtomicTarget> targets = getAcquireTargets(location);
    for (const AtomicTarget &target : targets) {
      const Instruction *sync_target =
          target.is_fence_target ? target.anchor : target.target;
      if (!sync_target) {
        continue;
      }
      if (m_mhp.getThreadID(release_candidates.front().first) ==
          m_mhp.getThreadID(sync_target)) {
        continue;
      }
      size_t before = m_sync_with.size();
      addSyncEdge(release_candidates.front().first, sync_target);
      bool modeled = m_sync_with.size() != before;
      if (!modeled) {
        ++m_deferred_sync_counts["atomic_release_candidate_unresolved"];
        continue;
      }
      if (has_release_sequence) {
        ++release_sequence_edges;
        ++m_deferred_sync_counts["atomic_release_sequence_edges_modeled"];
      } else if (target.is_fence_target ||
                 CppAtomics::isFence(release_candidates.front().first)) {
        ++mixed_fence_atomic_edges;
        ++m_deferred_sync_counts["atomic_mixed_fence_edges_modeled"];
      } else {
        ++direct_atomic_edges;
        ++m_deferred_sync_counts["atomic_direct_edges_modeled"];
      }
    }
  }

  size_t call_once_edges = 0;
  size_t deferred_call_once_relations = 0;
  for (size_t i = 0; i < call_once_ops.size(); ++i) {
    for (size_t j = i + 1; j < call_once_ops.size(); ++j) {
      if (!sameOnceFlag(call_once_ops[i], call_once_ops[j])) {
        continue;
      }
      bool emitted = false;
      for (const Instruction *call_once :
           {call_once_ops[i], call_once_ops[j]}) {
        const auto *cb = dyn_cast<CallBase>(call_once);
        const Function *callable = findCallOnceCallable(cb);
        if (!callable) {
          continue;
        }
        for (const Instruction &callback_inst : instructions(*callable)) {
          for (const Instruction *suffix :
               collectThreadSuffixInstructions(call_once)) {
            if (suffix == call_once) {
              continue;
            }
            addExplicitHBPair(&callback_inst, suffix);
            emitted = true;
          }
        }
      }
      if (emitted) {
        ++call_once_edges;
      } else {
        ++deferred_call_once_relations;
      }
    }
  }

  size_t latch_edges = 0;
  size_t deferred_latch_relations = 0;
  auto countMatchingLatches = [&](const Instruction *inst,
                                  const std::vector<const Instruction *> &candidates) {
    size_t count = 0;
    for (const Instruction *candidate : candidates) {
      if (sameLatch(inst, candidate)) {
        ++count;
      }
    }
    return count;
  };
  for (const Instruction *countdown : latch_countdowns) {
    for (const Instruction *wait : latch_waits) {
      if (!sameLatch(countdown, wait)) {
        continue;
      }
      if (countMatchingLatches(countdown, latch_countdowns) == 1 &&
          countMatchingLatches(wait, latch_waits) == 1) {
        size_t before = m_sync_with.size();
        addSyncEdge(countdown, wait);
        if (m_sync_with.size() != before) {
          ++latch_edges;
        }
      } else {
        ++deferred_latch_relations;
      }
    }
  }

  size_t barrier_edges = 0;
  size_t deferred_barrier_relations = 0;
  auto hasAmbiguousSplitPhaseBarrier = [&](const Instruction *inst) {
    std::unordered_map<mhp::ThreadID, size_t> arrive_counts;
    std::unordered_map<mhp::ThreadID, size_t> wait_counts;
    for (const Instruction *candidate : barrier_arrives) {
      if (sameBarrier(inst, candidate)) {
        ++arrive_counts[m_mhp.getThreadID(candidate)];
      }
    }
    for (const Instruction *candidate : barrier_waits) {
      if (sameBarrier(inst, candidate)) {
        ++wait_counts[m_mhp.getThreadID(candidate)];
      }
    }
    for (const auto &entry : arrive_counts) {
      if (entry.second > 1) {
        return true;
      }
    }
    for (const auto &entry : wait_counts) {
      if (entry.second > 1) {
        return true;
      }
    }
    return false;
  };
  for (const Instruction *arrive : barrier_arrives) {
    for (const Instruction *wait : barrier_waits) {
      if (!sameBarrier(arrive, wait) ||
          m_mhp.getThreadID(arrive) == m_mhp.getThreadID(wait)) {
        continue;
      }
      if (!hasAmbiguousSplitPhaseBarrier(arrive) &&
          !hasAmbiguousSplitPhaseBarrier(wait)) {
        size_t before = m_sync_with.size();
        addSyncEdge(arrive, wait);
        if (m_sync_with.size() != before) {
          ++barrier_edges;
        }
      } else {
        ++deferred_barrier_relations;
      }
    }
  }

  size_t omp_task_dependency_edges = 0;
  size_t omp_task_exclusion_relations = 0;
  size_t omp_task_unknown_relations = 0;
  std::unique_ptr<OpenMP::OpenMPSemantics> owned_semantics;
  const OpenMP::OpenMPSemantics *semantics = m_mhp.getOpenMPSemantics();
  if (!semantics) {
    owned_semantics = std::make_unique<OpenMP::OpenMPSemantics>(m_module);
    owned_semantics->analyze();
    semantics = owned_semantics.get();
  }

  for (const auto &entry : semantics->getRelations()) {
    const OpenMP::Task *lhs = entry.first.first;
    const OpenMP::Task *rhs = entry.first.second;
    const concurrency::Relation &relation = entry.second;
    if (!lhs || !rhs || !lhs->task_create || !rhs->task_create) {
      continue;
    }
    if (relation.kind == concurrency::RelationKind::MutuallyExclusive) {
      ++omp_task_exclusion_relations;
      continue;
    }
    if (relation.kind == concurrency::RelationKind::UnknownDueToModelGap) {
      ++omp_task_unknown_relations;
      continue;
    }
    if (relation.kind != concurrency::RelationKind::MustHappenBefore &&
        relation.kind != concurrency::RelationKind::SelectiveHappenBefore) {
      continue;
    }

    size_t before = m_sync_with.size();
    addSyncEdge(lhs->task_create, rhs->task_create);
    if (m_sync_with.size() != before) {
      ++omp_task_dependency_edges;
    }
  }

  m_deferred_sync_counts["atomic_release_candidates"] = release_ops.size();
  m_deferred_sync_counts["atomic_acquire_candidates"] = acquire_ops.size();
  m_deferred_sync_counts["atomic_direct_sync_edges"] = direct_atomic_edges;
  m_deferred_sync_counts["atomic_direct_relations_deferred"] =
      deferred_direct_atomic_relations;
  m_deferred_sync_counts["atomic_release_sequence_sync_edges"] =
      release_sequence_edges;
  m_deferred_sync_counts["atomic_release_sequence_relations_deferred"] =
      deferred_release_sequence_relations;
  m_deferred_sync_counts["atomic_mixed_fence_sync_edges"] =
      mixed_fence_atomic_edges;
  m_deferred_sync_counts["atomic_mixed_fence_relations_deferred"] =
      deferred_mixed_fence_relations;
  m_deferred_sync_counts["call_once_ops"] = call_once_ops.size();
  m_deferred_sync_counts["call_once_sync_edges"] = call_once_edges;
  m_deferred_sync_counts["call_once_relations_deferred"] =
      deferred_call_once_relations;
  m_deferred_sync_counts["latch_ops"] =
      latch_countdowns.size() + latch_waits.size();
  m_deferred_sync_counts["latch_sync_edges"] = latch_edges;
  m_deferred_sync_counts["latch_relations_deferred"] =
      deferred_latch_relations;
  m_deferred_sync_counts["barrier_ops"] =
      barrier_arrives.size() + barrier_waits.size();
  m_deferred_sync_counts["barrier_sync_edges"] = barrier_edges;
  m_deferred_sync_counts["barrier_relations_deferred"] =
      deferred_barrier_relations;
  m_deferred_sync_counts["omp_task_api_ops"] = omp_task_ops.size();
  m_deferred_sync_counts["omp_task_dependency_edges"] =
      omp_task_dependency_edges;
  m_deferred_sync_counts["omp_task_exclusion_relations"] =
      omp_task_exclusion_relations;
  m_deferred_sync_counts["omp_task_unknown_relations"] =
      omp_task_unknown_relations;
  m_deferred_sync_counts["omp_semantic_entities"] =
      semantics->getSemanticEntities().size();
  m_deferred_sync_counts["omp_semantic_events"] =
      semantics->getSemanticEvents().size();
  for (const auto &entry : semantics->getDeferredReasonCounts()) {
    m_deferred_sync_counts[entry.first] += entry.second;
  }

  errs() << "HB deferred sync candidates: atomics="
         << m_deferred_sync_counts["atomic_release_candidates"] +
                m_deferred_sync_counts["atomic_acquire_candidates"]
         << ", atomic_sync_edges="
         << m_deferred_sync_counts["atomic_direct_sync_edges"]
         << ", call_once=" << m_deferred_sync_counts["call_once_ops"]
         << ", call_once_edges="
         << m_deferred_sync_counts["call_once_sync_edges"]
         << ", latches=" << m_deferred_sync_counts["latch_ops"]
         << ", latch_edges=" << m_deferred_sync_counts["latch_sync_edges"]
         << ", barriers=" << m_deferred_sync_counts["barrier_ops"]
         << ", barrier_edges=" << m_deferred_sync_counts["barrier_sync_edges"]
         << ", omp_task_ops=" << m_deferred_sync_counts["omp_task_api_ops"]
         << ", omp_task_edges=" << omp_task_dependency_edges << "\n";
}

bool HappensBeforeAnalysis::canReach(const mhp::SyncNode *start,
                                     const mhp::SyncNode *end) const {
  if (!start || !end) {
    return false;
  }

  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  if (tfg.hasReachabilityIndex()) {
    return tfg.canReach(start, end);
  }

  std::deque<const mhp::SyncNode *> worklist;
  std::unordered_set<const mhp::SyncNode *> visited;
  worklist.push_back(start);
  visited.insert(start);

  while (!worklist.empty()) {
    const mhp::SyncNode *current = worklist.front();
    worklist.pop_front();
    if (current == end) {
      return true;
    }
    for (mhp::SyncNode *succ : current->getSuccessors()) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

bool HappensBeforeAnalysis::canReachWithHB(const mhp::SyncNode *start,
                                           const mhp::SyncNode *end) const {
  if (!start || !end) {
    return false;
  }

  std::deque<const mhp::SyncNode *> worklist;
  std::unordered_set<const mhp::SyncNode *> visited;
  worklist.push_back(start);
  visited.insert(start);

  while (!worklist.empty()) {
    const mhp::SyncNode *current = worklist.front();
    worklist.pop_front();
    if (current == end) {
      return true;
    }

    for (mhp::SyncNode *succ : current->getSuccessors()) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }

    auto extra_it = m_extra_hb_successors.find(current);
    if (extra_it == m_extra_hb_successors.end()) {
      continue;
    }
    for (const mhp::SyncNode *succ : extra_it->second) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

bool HappensBeforeAnalysis::canReachExplicitHB(const Instruction *from,
                                               const Instruction *to) const {
  if (!from || !to || from == to) {
    return false;
  }

  std::deque<const Instruction *> worklist;
  std::unordered_set<const Instruction *> visited;
  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const Instruction *current = worklist.front();
    worklist.pop_front();
    if (current == to) {
      return true;
    }

    for (const auto &pair : m_explicit_hb_pairs) {
      if (pair.first != current) {
        continue;
      }
      if (visited.insert(pair.second).second) {
        worklist.push_back(pair.second);
      }
    }
  }

  return false;
}

bool HappensBeforeAnalysis::hasProgramOrder(const Instruction *A,
                                            const Instruction *B) const {
  if (!A || !B || A == B) {
    return false;
  }

  mhp::ThreadID a_tid = m_mhp.getThreadID(A);
  mhp::ThreadID b_tid = m_mhp.getThreadID(B);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::vector<mhp::SyncNode *> start_nodes =
      a_tid == std::numeric_limits<mhp::ThreadID>::max() ? tfg.getNodes(A)
                                                         : tfg.getNodes(A, a_tid);
  std::vector<mhp::SyncNode *> end_nodes =
      b_tid == std::numeric_limits<mhp::ThreadID>::max() ? tfg.getNodes(B)
                                                         : tfg.getNodes(B, b_tid);
  if (start_nodes.empty() || end_nodes.empty()) {
    return false;
  }

  for (mhp::SyncNode *start : start_nodes) {
    for (mhp::SyncNode *end : end_nodes) {
      if (!canReach(start, end)) {
        return false;
      }
    }
  }
  return true;
}

bool HappensBeforeAnalysis::isInstructionThreadAmbiguous(
    const Instruction *inst) const {
  return !inst ||
         m_mhp.getThreadID(inst) == std::numeric_limits<mhp::ThreadID>::max();
}

bool HappensBeforeAnalysis::sameAtomicLocation(
    const Instruction *store_inst, const Instruction *load_inst) const {
  if (!store_inst || !load_inst) {
    return false;
  }

  const AtomicLocationKey lhs = getExactAtomicLocation(store_inst);
  const AtomicLocationKey rhs = getExactAtomicLocation(load_inst);
  if (!lhs.base || !rhs.base) {
    return false;
  }
  if (lhs == rhs) {
    return true;
  }
  if (lhs.base == rhs.base && lhs.has_precise_offset && rhs.has_precise_offset) {
    return false;
  }

  const Value *p1 = CppAtomics::getAtomicPointer(store_inst);
  const Value *p2 = CppAtomics::getAtomicPointer(load_inst);
  if (!p1 || !p2) {
    return false;
  }
  p1 = p1->stripPointerCasts();
  p2 = p2->stripPointerCasts();
  if (p1 == p2) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mayAlias(p1, p2);
}

bool HappensBeforeAnalysis::isFenceAnchorCompatibleInstruction(
    const Instruction *inst) const {
  if (!inst) {
    return false;
  }
  if (isa<DbgInfoIntrinsic>(inst) || isa<PHINode>(inst)) {
    return true;
  }
  if (const auto *cb = dyn_cast<CallBase>(inst)) {
    return cb->doesNotAccessMemory();
  }
  return !inst->mayReadOrWriteMemory();
}

const Instruction *HappensBeforeAnalysis::findNearestAtomicInBlock(
    const Instruction *inst, bool search_backward, bool require_load_like,
    bool require_store_like) const {
  if (!inst) {
    return nullptr;
  }
  if (!inst->getParent()) {
    return nullptr;
  }

  const Instruction *cursor = inst;
  while (true) {
    cursor = search_backward ? cursor->getPrevNode() : cursor->getNextNode();
    if (!cursor) {
      return nullptr;
    }
    if (CppAtomics::isFence(cursor)) {
      return nullptr;
    }
    if (CppAtomics::isAtomic(cursor)) {
      if (require_load_like && !CppAtomics::isLoad(cursor)) {
        return nullptr;
      }
      if (require_store_like && !CppAtomics::isStore(cursor)) {
        return nullptr;
      }
      return cursor;
    }
    if (!isFenceAnchorCompatibleInstruction(cursor)) {
      return nullptr;
    }
  }
}

const Instruction *
HappensBeforeAnalysis::getSinglePrecedingAtomicLoad(const Instruction *inst) const {
  return findNearestAtomicInBlock(inst, true, true, false);
}

const Instruction *
HappensBeforeAnalysis::getSingleFollowingAcquireFence(const Instruction *inst) const {
  const Instruction *next = inst ? inst->getNextNode() : nullptr;
  while (next && isFenceAnchorCompatibleInstruction(next)) {
    if (CppAtomics::isFence(next)) {
      break;
    }
    next = next->getNextNode();
  }
  if (!next || !CppAtomics::isFence(next)) {
    return nullptr;
  }
  if (!(CppAtomics::isFenceAcquire(next) || CppAtomics::isFenceAcqRel(next) ||
        CppAtomics::isFenceSeqCst(next))) {
    return nullptr;
  }
  return next;
}

const PostDominatorTree &
HappensBeforeAnalysis::getPostDominatorTree(const Function *func) const {
  auto it = m_post_dom_cache.find(func);
  if (it != m_post_dom_cache.end()) {
    return *(it->second);
  }

  auto pdt = std::make_unique<PostDominatorTree>();
  pdt->recalculate(*const_cast<Function *>(func));
  auto *pdt_ptr = pdt.get();
  m_post_dom_cache[func] = std::move(pdt);
  return *pdt_ptr;
}

bool HappensBeforeAnalysis::hasSupportedAtomicWitness(
    const Instruction *inst) const {
  if (!inst) {
    return false;
  }
  if (hasBranchWitness(inst)) {
    return true;
  }
  return getSingleFollowingAcquireFence(inst) != nullptr && hasBranchWitness(inst);
}

bool HappensBeforeAnalysis::samePromiseFuturePair(
    const Instruction *promise, const Instruction *future) const {
  const CallBase *p = dyn_cast<CallBase>(promise);
  const CallBase *f = dyn_cast<CallBase>(future);
  if (!p || !f || p->arg_size() == 0 || f->arg_size() == 0) {
    return false;
  }

  const Value *promise_base = p->getArgOperand(0)->stripPointerCasts();
  const Value *future_base = f->getArgOperand(0)->stripPointerCasts();
  const Value *promise_state = traceSharedState(promise_base);
  const Value *future_state = traceSharedState(future_base);
  if (!promise_state || !future_state) {
    return false;
  }
  if (promise_state == future_state) {
    return true;
  }
  return m_alias_analysis &&
         m_alias_analysis->mustAlias(promise_state, future_state);
}

bool HappensBeforeAnalysis::sameOnceFlag(const Instruction *call1,
                                         const Instruction *call2) const {
  const CallBase *c1 = dyn_cast<CallBase>(call1);
  const CallBase *c2 = dyn_cast<CallBase>(call2);
  if (!c1 || !c2 || c1->arg_size() < 1 || c2->arg_size() < 1) {
    return false;
  }

  const Value *flag1 = c1->getArgOperand(0)->stripPointerCasts();
  const Value *flag2 = c2->getArgOperand(0)->stripPointerCasts();
  if (flag1 == flag2) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mustAlias(flag1, flag2);
}

bool HappensBeforeAnalysis::sameLatch(const Instruction *inst1,
                                      const Instruction *inst2) const {
  const CallBase *c1 = dyn_cast<CallBase>(inst1);
  const CallBase *c2 = dyn_cast<CallBase>(inst2);
  if (!c1 || !c2 || c1->arg_size() < 1 || c2->arg_size() < 1) {
    return false;
  }

  const Value *latch1 = c1->getArgOperand(0)->stripPointerCasts();
  const Value *latch2 = c2->getArgOperand(0)->stripPointerCasts();
  if (latch1 == latch2) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mustAlias(latch1, latch2);
}

bool HappensBeforeAnalysis::sameBarrier(const Instruction *inst1,
                                        const Instruction *inst2) const {
  const CallBase *c1 = dyn_cast<CallBase>(inst1);
  const CallBase *c2 = dyn_cast<CallBase>(inst2);
  if (!c1 || !c2 || c1->arg_size() < 1 || c2->arg_size() < 1) {
    return false;
  }

  const Value *barrier1 = c1->getArgOperand(0)->stripPointerCasts();
  const Value *barrier2 = c2->getArgOperand(0)->stripPointerCasts();
  if (barrier1 == barrier2) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mustAlias(barrier1, barrier2);
}

const Value *HappensBeforeAnalysis::traceSharedState(const Value *value) const {
  if (!value) {
    return nullptr;
  }

  const Value *cache_key = value->stripPointerCasts();
  auto cache_it = m_shared_state_trace_cache.find(cache_key);
  if (cache_it != m_shared_state_trace_cache.end()) {
    return cache_it->second;
  }

  std::vector<const Value *> worklist = {value};
  std::unordered_set<const Value *> visited;
  const Value *resolved = nullptr;
  bool ambiguous = false;

  while (!worklist.empty()) {
    const Value *current = worklist.back();
    worklist.pop_back();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    auto mapped = m_future_shared_state.find(current);
    if (mapped != m_future_shared_state.end()) {
      const Value *candidate = mapped->second
                                   ? mapped->second->stripPointerCasts()
                                   : nullptr;
      if (!resolved) {
        resolved = candidate;
      } else if (candidate && resolved != candidate &&
                 !(m_alias_analysis &&
                   m_alias_analysis->mustAlias(resolved, candidate))) {
        ambiguous = true;
      }
      continue;
    }

    const Value *stripped = current->stripPointerCasts();
    if (stripped != current) {
      worklist.push_back(stripped);
    }

    if (isa<AllocaInst>(current) || isa<GlobalValue>(current)) {
      const Value *candidate = current->stripPointerCasts();
      if (!resolved) {
        resolved = candidate;
      } else if (resolved != candidate &&
                 !(m_alias_analysis &&
                   m_alias_analysis->mustAlias(resolved, candidate))) {
        ambiguous = true;
      }
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(current)) {
      const Function *parent = arg->getParent();
      bool expanded = false;
      if (parent) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (cb && arg->getArgNo() < cb->arg_size()) {
            worklist.push_back(cb->getArgOperand(arg->getArgNo()));
            expanded = true;
          }
        }

        for (const Function &func : m_module) {
          for (const BasicBlock &bb : func) {
            for (const Instruction &inst : bb) {
              const auto *cb = dyn_cast<CallBase>(&inst);
              if (!cb || arg->getArgNo() >= cb->arg_size()) {
                continue;
              }
              const Value *called = cb->getCalledOperand();
              if (called && called->stripPointerCasts() == parent) {
                worklist.push_back(cb->getArgOperand(arg->getArgNo()));
                expanded = true;
              }
            }
          }
        }
      }
      if (!expanded) {
        const Value *candidate = current->stripPointerCasts();
        if (!resolved) {
          resolved = candidate;
        } else if (resolved != candidate &&
                   !(m_alias_analysis &&
                     m_alias_analysis->mustAlias(resolved, candidate))) {
          ambiguous = true;
        }
      }
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      for (const User *user : load->getPointerOperand()->users()) {
        if (const auto *store = dyn_cast<StoreInst>(user)) {
          if (store->getPointerOperand() == load->getPointerOperand()) {
            worklist.push_back(store->getValueOperand());
          }
        }
      }
    } else if (const auto *store = dyn_cast<StoreInst>(current)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
    } else if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
    } else if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
    } else if (const auto *cb = dyn_cast<CallBase>(current)) {
      if (cb->arg_size() >= 1) {
        worklist.push_back(cb->getArgOperand(0));
      }
    } else if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Use &operand : inst->operands()) {
        worklist.push_back(operand.get());
      }
    }
  }

  if (ambiguous) {
    resolved = nullptr;
  }

  m_shared_state_trace_cache[cache_key] = resolved;
  return resolved;
}

bool HappensBeforeAnalysis::happensBefore(const Instruction *A,
                                          const Instruction *B) const {
  if (!A || !B) {
    return false;
  }
  if (A == B) {
    return false;
  }
  auto key = std::make_pair(A, B);
  auto cache_it = m_hb_cache.find(key);
  if (cache_it != m_hb_cache.end()) {
    return cache_it->second;
  }

  if (m_explicit_hb_pairs.count(key) != 0) {
    m_hb_cache[key] = true;
    return true;
  }
  if (canReachExplicitHB(A, B)) {
    m_hb_cache[key] = true;
    return true;
  }

  mhp::ThreadID a_tid = m_mhp.getThreadID(A);
  mhp::ThreadID b_tid = m_mhp.getThreadID(B);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::vector<mhp::SyncNode *> start_nodes =
      a_tid == std::numeric_limits<mhp::ThreadID>::max() ? tfg.getNodes(A)
                                                         : tfg.getNodes(A, a_tid);
  std::vector<mhp::SyncNode *> end_nodes =
      b_tid == std::numeric_limits<mhp::ThreadID>::max() ? tfg.getNodes(B)
                                                         : tfg.getNodes(B, b_tid);
  if (start_nodes.empty() || end_nodes.empty()) {
    m_hb_cache[key] = false;
    return false;
  }

  for (mhp::SyncNode *start : start_nodes) {
    for (mhp::SyncNode *end : end_nodes) {
      if (!canReachWithHB(start, end)) {
        m_hb_cache[key] = false;
        return false;
      }
    }
  }

  m_hb_cache[key] = true;
  return true;
}

} // namespace lotus
