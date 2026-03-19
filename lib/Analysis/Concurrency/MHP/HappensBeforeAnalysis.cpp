#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/OpenMP/OpenMPSemantics.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <set>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

HappensBeforeAnalysis::HappensBeforeAnalysis(Module &module,
                                             mhp::MHPAnalysis &mhp)
    : m_module(module), m_mhp(mhp) {}

void HappensBeforeAnalysis::analyze() {
  m_hb_cache.clear();
  m_future_shared_state.clear();
  m_shared_state_trace_cache.clear();
  m_deferred_sync_counts.clear();
  m_atomic_instructions.clear();
  m_atomic_sync_witnesses.clear();
  m_atomic_hb_pairs.clear();
  m_sync_with.clear();
  m_extra_hb_successors.clear();

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
  if (from_tid == std::numeric_limits<mhp::ThreadID>::max() ||
      to_tid == std::numeric_limits<mhp::ThreadID>::max()) {
    return;
  }

  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  for (mhp::SyncNode *from_node : tfg.getNodes(from, from_tid)) {
    auto &succs = m_extra_hb_successors[from_node];
    for (mhp::SyncNode *to_node : tfg.getNodes(to, to_tid)) {
      if (std::find(succs.begin(), succs.end(), to_node) == succs.end()) {
        succs.push_back(to_node);
      }
    }
  }
}

void HappensBeforeAnalysis::computeAtomicHappensBefore() {
  errs() << "Computing Atomic Happens-Before...\n";

  using namespace CppAtomics;

  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if (isAtomic(&*I)) {
        m_atomic_instructions.push_back(&*I);
      }
    }
  }

  std::vector<const Instruction *> release_fences;
  std::vector<const Instruction *> acquire_fences;
  for (const Instruction *inst : m_atomic_instructions) {
    if (isFenceRelease(inst) || isFenceAcqRel(inst) || isFenceSeqCst(inst)) {
      release_fences.push_back(inst);
    }
    if (isFenceAcquire(inst) || isFenceAcqRel(inst) || isFenceSeqCst(inst)) {
      acquire_fences.push_back(inst);
    }
  }

  for (const Instruction *release_fence : release_fences) {
    std::vector<const Instruction *> release_witnesses =
        collectFenceWitnesses(release_fence,
                              /*require_release_semantics=*/true);
    if (release_witnesses.empty()) {
      continue;
    }

    for (const Instruction *acquire_fence : acquire_fences) {
      std::vector<const Instruction *> acquire_witnesses =
          collectFenceWitnesses(acquire_fence,
                                /*require_release_semantics=*/false);
      if (acquire_witnesses.empty()) {
        continue;
      }

      bool matched = false;
      for (const Instruction *release_inst : release_witnesses) {
        for (const Instruction *acquire_inst : acquire_witnesses) {
          if (!hasReleaseSemantics(release_inst) ||
              !hasAcquireSemantics(acquire_inst)) {
            continue;
          }
          if (!atomicLocationsMayAlias(release_inst, acquire_inst)) {
            continue;
          }
          if (m_atomic_hb_pairs.emplace(release_fence, acquire_fence).second) {
            AtomicSyncWitness witness;
            witness.release = release_inst;
            witness.acquire = acquire_inst;
            witness.location = CppAtomics::getAtomicPointer(release_inst);
            m_atomic_sync_witnesses.push_back(witness);
            addExtraHBEdge(release_fence, acquire_fence);
          }
          matched = true;
          break;
        }
        if (matched) {
          break;
        }
      }
    }
  }

  errs() << "Recorded " << m_atomic_hb_pairs.size()
         << " atomic HB edges from fence witnesses.\n";
}

std::vector<const Instruction *>
HappensBeforeAnalysis::collectFenceWitnesses(
    const Instruction *fence, bool require_release_semantics) const {
  std::vector<const Instruction *> witnesses;
  if (!fence) {
    return witnesses;
  }

  mhp::ThreadID fence_tid = m_mhp.getThreadID(fence);
  if (fence_tid == std::numeric_limits<mhp::ThreadID>::max()) {
    return witnesses;
  }

  for (const Instruction *inst : m_atomic_instructions) {
    if (inst == fence || CppAtomics::isFence(inst)) {
      continue;
    }
    if (m_mhp.getThreadID(inst) != fence_tid) {
      continue;
    }

    if (require_release_semantics) {
      if (!CppAtomics::hasReleaseSemantics(inst) ||
          !CppAtomics::isStore(inst)) {
        continue;
      }
      if (!hasProgramOrder(inst, fence)) {
        continue;
      }
    } else {
      if (!CppAtomics::hasAcquireSemantics(inst) || !CppAtomics::isLoad(inst)) {
        continue;
      }
      if (!hasProgramOrder(fence, inst)) {
        continue;
      }
    }

    if (!CppAtomics::getAtomicPointer(inst)) {
      continue;
    }
    witnesses.push_back(inst);
  }

  return witnesses;
}

bool HappensBeforeAnalysis::atomicLocationsMayAlias(
    const Instruction *lhs, const Instruction *rhs) const {
  const Value *lhs_ptr = CppAtomics::getAtomicPointer(lhs);
  const Value *rhs_ptr = CppAtomics::getAtomicPointer(rhs);
  if (!lhs_ptr || !rhs_ptr) {
    return false;
  }
  lhs_ptr = lhs_ptr->stripPointerCasts();
  rhs_ptr = rhs_ptr->stripPointerCasts();
  if (lhs_ptr == rhs_ptr) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mayAlias(lhs_ptr, rhs_ptr);
}

void HappensBeforeAnalysis::buildSynchronizesWith() {
  using namespace CppAtomics;

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

  std::set<std::pair<const Instruction *, const Instruction *>> seen_sync_edges;
  auto addSyncEdge = [&](const Instruction *from, const Instruction *to) {
    if (!from || !to || from == to) {
      return;
    }
    if (seen_sync_edges.emplace(from, to).second) {
      m_sync_with.emplace_back(from, to);
      addExtraHBEdge(from, to);
    }
  };

  ThreadAPI *threadAPI = ThreadAPI::getThreadAPI();

  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;

      if (isAtomic(inst)) {
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
        break;
      case ThreadAPI::TD_LATCH_WAIT:
        latch_waits.push_back(inst);
        break;
      case ThreadAPI::TD_BARRIER_ARRIVE:
      case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
        barrier_arrives.push_back(inst);
        if (type == ThreadAPI::TD_BARRIER_ARRIVE_WAIT) {
          barrier_waits.push_back(inst);
        }
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
  m_deferred_sync_counts["call_once_ops"] = call_once_ops.size();
  m_deferred_sync_counts["latch_ops"] =
      latch_countdowns.size() + latch_waits.size();
  m_deferred_sync_counts["barrier_ops"] =
      barrier_arrives.size() + barrier_waits.size();
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
         << ", call_once=" << m_deferred_sync_counts["call_once_ops"]
         << ", latches=" << m_deferred_sync_counts["latch_ops"]
         << ", barriers=" << m_deferred_sync_counts["barrier_ops"]
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
      const Instruction *succ_inst = succ->getInstruction();
      if (succ_inst && isInstructionThreadAmbiguous(succ_inst)) {
        continue;
      }
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
      const Instruction *succ_inst = succ->getInstruction();
      if (succ_inst && isInstructionThreadAmbiguous(succ_inst)) {
        continue;
      }
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }

    auto extra_it = m_extra_hb_successors.find(current);
    if (extra_it == m_extra_hb_successors.end()) {
      continue;
    }
    for (const mhp::SyncNode *succ : extra_it->second) {
      const Instruction *succ_inst = succ->getInstruction();
      if (succ_inst && isInstructionThreadAmbiguous(succ_inst)) {
        continue;
      }
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

bool HappensBeforeAnalysis::hasProgramOrder(const Instruction *A,
                                            const Instruction *B) const {
  if (!A || !B || A == B || isInstructionThreadAmbiguous(A) ||
      isInstructionThreadAmbiguous(B)) {
    return false;
  }

  mhp::ThreadID a_tid = m_mhp.getThreadID(A);
  mhp::ThreadID b_tid = m_mhp.getThreadID(B);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::vector<mhp::SyncNode *> start_nodes = tfg.getNodes(A, a_tid);
  std::vector<mhp::SyncNode *> end_nodes = tfg.getNodes(B, b_tid);
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
  const Value *p1 = CppAtomics::getAtomicPointer(store_inst);
  const Value *p2 = CppAtomics::getAtomicPointer(load_inst);
  if (!p1 || !p2) {
    return false;
  }
  if (p1->stripPointerCasts() == p2->stripPointerCasts()) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mayAlias(p1, p2);
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

  while (!worklist.empty()) {
    const Value *current = worklist.back();
    worklist.pop_back();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    auto mapped = m_future_shared_state.find(current);
    if (mapped != m_future_shared_state.end()) {
      resolved = mapped->second;
      break;
    }

    const Value *stripped = current->stripPointerCasts();
    if (stripped != current) {
      worklist.push_back(stripped);
    }

    if (isa<AllocaInst>(current) || isa<GlobalValue>(current)) {
      resolved = current;
      break;
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
        resolved = current;
        break;
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

  m_shared_state_trace_cache[cache_key] = resolved;
  return resolved;
}

bool HappensBeforeAnalysis::happensBefore(const Instruction *A,
                                          const Instruction *B) const {
  if (!A || !B) {
    return false;
  }
  if (A == B) {
    return true;
  }
  if (isInstructionThreadAmbiguous(A) || isInstructionThreadAmbiguous(B)) {
    return false;
  }

  auto key = std::make_pair(A, B);
  auto cache_it = m_hb_cache.find(key);
  if (cache_it != m_hb_cache.end()) {
    return cache_it->second;
  }

  mhp::ThreadID a_tid = m_mhp.getThreadID(A);
  mhp::ThreadID b_tid = m_mhp.getThreadID(B);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::vector<mhp::SyncNode *> start_nodes = tfg.getNodes(A, a_tid);
  std::vector<mhp::SyncNode *> end_nodes = tfg.getNodes(B, b_tid);
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
