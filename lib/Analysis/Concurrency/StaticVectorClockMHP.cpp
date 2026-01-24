/**
 * @file StaticVectorClockMHP.cpp
 * @brief Implementation of the static vector clock based MHP analysis.
 */

#include "Analysis/Concurrency/StaticVectorClockMHP.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

#include <deque>
#include <unordered_set>

using namespace llvm;
using namespace mhp;

StaticVectorClockMHP::StaticVectorClockMHP(Module &module)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()) {
  m_tfg = std::make_unique<ThreadFlowGraph>();
}

void StaticVectorClockMHP::analyze() {
  // Build a fresh thread-flow graph locally (no dependency on MHPAnalysis).
  buildThreadFlowGraph();

  buildStaticThreads();
  computeStaticVectorClocks();
  computeMHPPairs();
}

// === StaticVectorClock helpers =============================================

bool StaticVectorClockMHP::StaticVectorClock::mergeFrom(
    const StaticVectorClock &other) {
  bool changed = false;
  for (const auto &kv : other.entries) {
    auto &dest = entries[kv.first];
    for (const auto &elem : kv.second) {
      changed |= dest.insert(elem).second;
    }
  }
  return changed;
}

bool StaticVectorClockMHP::StaticVectorClock::leq(
    const StaticVectorClock &other) const {
  for (const auto &kv : entries) {
    auto it = other.entries.find(kv.first);
    if (it == other.entries.end()) {
      if (!kv.second.empty())
        return false;
      continue;
    }
    const auto &rhs = it->second;
    for (const auto &elem : kv.second) {
      if (rhs.find(elem) == rhs.end())
        return false;
    }
  }
  return true;
}

StaticVectorClockMHP::StaticVectorClock
StaticVectorClockMHP::initialClockFor(const StaticThread &st) const {
  StaticVectorClock init;
  LogicClockSet starter;
  starter.insert({LogicClockElem::Kind::Start, 0});
  init.entries[st.id] = std::move(starter);
  return init;
}

StaticThreadID StaticVectorClockMHP::getOrCreateStaticThread(const Context &ctx,
                                                             ThreadID base_tid,
                                                             const SyncNode *entry) {
  auto it = m_ctx_to_stid.find(ctx);
  if (it != m_ctx_to_stid.end())
    return it->second;

  StaticThreadID new_id = m_static_threads.size();
  StaticThread st;
  st.id = new_id;
  st.ctx = ctx;
  st.base_tid = base_tid;
  st.entry = entry;
  m_static_threads.push_back(st);
  m_ctx_to_stid[ctx] = new_id;
  return new_id;
}

void StaticVectorClockMHP::buildStaticThreads() {
  m_static_threads.clear();
  m_ctx_to_stid.clear();
  m_node_to_static_thread.clear();
  m_inst_to_static_thread.clear();
  m_node_clocks.clear();

  if (!m_tfg)
    return;

  const SyncNode *main_entry = m_tfg->getThreadEntryNode(0);
  Context root;
  StaticThreadID root_id = getOrCreateStaticThread(root, 0, main_entry);

  std::deque<StaticThreadID> worklist;
  worklist.push_back(root_id);

  while (!worklist.empty()) {
    StaticThreadID stid = worklist.front();
    worklist.pop_front();

    StaticThread &st = m_static_threads[stid];
    if (!st.entry)
      continue;

    std::unordered_set<const SyncNode *> visited;
    std::deque<const SyncNode *> nq;
    nq.push_back(st.entry);
    visited.insert(st.entry);

    while (!nq.empty()) {
      const SyncNode *node = nq.front();
      nq.pop_front();

      st.nodes.push_back(node);
      m_node_to_static_thread[node] = stid;
      if (const Instruction *inst = node->getInstruction()) {
        m_inst_to_static_thread[inst] = stid;
      }
      if (m_node_clocks.find(node) == m_node_clocks.end()) {
        m_node_clocks.insert(std::make_pair(node, initialClockFor(st)));
      }

      // Handle fork to spawn new static thread context
      if (node->getType() == SyncNodeType::THREAD_FORK) {
        ThreadID child_tid = node->getForkedThread();
        const SyncNode *child_entry = m_tfg->getThreadEntryNode(child_tid);
        if (child_entry) {
          Context child_ctx = st.ctx;
          child_ctx.fork_sites.push_back(node->getNodeID());
          StaticThreadID child_stid =
              getOrCreateStaticThread(child_ctx, child_tid, child_entry);
          // Only enqueue if first time we saw it
          if (m_static_threads[child_stid].nodes.empty()) {
            worklist.push_back(child_stid);
          }
        }
      }

      // Traverse successors in the same base thread to build the intra-thread CFG
      for (auto *succ : node->getSuccessors()) {
        if (succ->getThreadID() != st.base_tid)
          continue;
        if (visited.insert(succ).second) {
          nq.push_back(succ);
        }
      }
    }
  }
}

StaticVectorClockMHP::StaticVectorClock
StaticVectorClockMHP::mergePredecessorClocks(const SyncNode *node) const {
  StaticVectorClock merged;
  if (!node)
    return merged;

  for (auto *pred : node->getPredecessors()) {
    auto it = m_node_clocks.find(pred);
    if (it != m_node_clocks.end()) {
      merged.mergeFrom(it->second);
    }
  }
  return merged;
}

void StaticVectorClockMHP::addEventToClock(const SyncNode *node,
                                           StaticVectorClock &sv) const {
  auto st_it = m_node_to_static_thread.find(node);
  if (st_it == m_node_to_static_thread.end())
    return;

  StaticThreadID stid = st_it->second;
  LogicClockElem elem{LogicClockElem::Kind::Node, node->getNodeID()};
  sv.entries[stid].insert(elem);

  if (node->getType() == SyncNodeType::THREAD_EXIT) {
    sv.entries[stid].insert({LogicClockElem::Kind::Terminated, 0});
  }
}

bool StaticVectorClockMHP::transfer(const SyncNode *node) {
  if (!node)
    return false;

  // Vector Clock Update Rule:
  // 1. Merge (Join) vector clocks from all predecessors (max per thread component).
  //    VC_in(n) = max(VC_out(p)) for all p in preds(n)
  // 2. Increment the local thread's clock component for the current event.
  //    VC_out(n) = VC_in(n); VC_out(n)[tid]++
  // 3. Update the node's clock. Return true if changed (for fixpoint).

  StaticVectorClock incoming = mergePredecessorClocks(node);

  // If no predecessors, seed with the static thread's initial clock.
  if (incoming.entries.empty()) {
    auto st_it = m_node_to_static_thread.find(node);
    if (st_it != m_node_to_static_thread.end()) {
      incoming = initialClockFor(m_static_threads[st_it->second]);
    }
  }

  addEventToClock(node, incoming);

  StaticVectorClock &current = m_node_clocks[node];
  bool changed = !incoming.leq(current) || !current.leq(incoming);
  if (changed) {
    current = incoming;
  }
  return changed;
}

void StaticVectorClockMHP::computeStaticVectorClocks() {
  if (!m_tfg)
    return;

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &pair : m_node_clocks) {
      const SyncNode *node = pair.first;
      changed |= transfer(node);
    }
  }
}

bool StaticVectorClockMHP::happensBefore(const StaticVectorClock &lhs,
                                         const StaticVectorClock &rhs) const {
  // Returns true if lhs happens-before rhs (strictly ordered)
  // Logic: lhs <= rhs AND !(rhs <= lhs)
  return lhs.leq(rhs) && !rhs.leq(lhs);
}

bool StaticVectorClockMHP::happensBefore(const Instruction *i1,
                                         const Instruction *i2) const {
  if (!m_tfg)
    return false;

  if (!i1 || !i2 || i1 == i2)
    return false;

  if (isInstructionThreadAmbiguous(i1) || isInstructionThreadAmbiguous(i2)) {
    return false;
  }

  const SyncNode *n1 = m_tfg->getNode(i1);
  const SyncNode *n2 = m_tfg->getNode(i2);
  if (!n1 || !n2)
    return false;

  std::deque<const SyncNode *> worklist;
  std::unordered_set<const SyncNode *> visited;
  worklist.push_back(n1);
  visited.insert(n1);

  while (!worklist.empty()) {
    const SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == n2) {
      return true;
    }

    for (const SyncNode *succ : current->getSuccessors()) {
      const Instruction *succ_inst = succ->getInstruction();
      if (succ_inst && isInstructionThreadAmbiguous(succ_inst)) {
        continue;
      }
      if (current->getThreadID() == succ->getThreadID() &&
          !isMustIntraThreadEdge(current, succ)) {
        continue;
      }
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

void StaticVectorClockMHP::computeMHPPairs() {
  std::vector<const Instruction *> all_insts;
  all_insts.reserve(m_inst_to_thread.size());
  for (Function &func : m_module) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      if (m_inst_to_thread.count(inst)) {
        all_insts.push_back(inst);
      }
    }
  }

  for (size_t i = 0; i < all_insts.size(); ++i) {
    for (size_t j = i + 1; j < all_insts.size(); ++j) {
      const Instruction *a = all_insts[i];
      const Instruction *b = all_insts[j];

      if (happensBefore(a, b) || happensBefore(b, a)) {
        continue;
      }

      m_mhp_pairs.insert({a, b});
    }
  }
}

bool StaticVectorClockMHP::mayHappenInParallel(const Instruction *i1,
                                               const Instruction *i2) const {
  if (!i1 || !i2 || i1 == i2)
    return false;

  if (m_mhp_pairs.count({i1, i2}) || m_mhp_pairs.count({i2, i1}))
    return true;

  return !happensBefore(i1, i2) && !happensBefore(i2, i1);
}

void StaticVectorClockMHP::printStatistics(raw_ostream &os) const {
  size_t num_static_threads = m_static_threads.size();
  size_t num_nodes = m_node_clocks.size();

  os << "SVC-MHP Statistics:\n";
  os << "===================\n";
  os << "Static Threads: " << num_static_threads << "\n";
  os << "TFG Nodes:      " << num_nodes << "\n";
  os << "MHP Pairs:      " << m_mhp_pairs.size() << "\n";
}

void StaticVectorClockMHP::printResults(raw_ostream &os) const {
  printStatistics(os);
  os << "\nSample MHP pairs (up to 10):\n";
  int shown = 0;
  for (const auto &p : m_mhp_pairs) {
    os << "MHP: ";
    p.first->print(os);
    os << " || ";
    p.second->print(os);
    os << "\n";
    if (++shown >= 10) {
      if (m_mhp_pairs.size() > 10) {
        os << "... (" << (m_mhp_pairs.size() - 10) << " more)\n";
      }
      break;
    }
  }
}

// === Thread-flow graph construction (self contained, no MHPAnalysis) ========

void StaticVectorClockMHP::buildThreadFlowGraph() {
  m_tfg = std::make_unique<ThreadFlowGraph>();

  Function *main_func = m_module.getFunction("main");
  if (!main_func) {
    errs() << "SVC-MHP: no main function found\n";
    return;
  }

  m_tfg->addThread(0, main_func);
  processFunction(main_func, 0);
}

void StaticVectorClockMHP::processFunction(const Function *func, ThreadID tid) {
  if (!func || func->isDeclaration())
    return;

  auto &visited = m_visited_functions_by_thread[tid];
  if (!visited.insert(func).second)
    return;

  // --- Pass 1: Create all nodes for this function ---
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      mapInstructionToThread(&inst, tid);
      SyncNodeType node_type = SyncNodeType::REGULAR_INST;

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        (void)cb;
        if (m_thread_api->isTDFork(&inst)) {
          node_type = SyncNodeType::THREAD_FORK;
        } else if (m_thread_api->isTDJoin(&inst)) {
          node_type = SyncNodeType::THREAD_JOIN;
        } else if (m_thread_api->isTDAcquire(&inst)) {
          node_type = SyncNodeType::LOCK_ACQUIRE;
        } else if (m_thread_api->isTDRelease(&inst)) {
          node_type = SyncNodeType::LOCK_RELEASE;
        } else if (m_thread_api->isTDExit(&inst)) {
          node_type = SyncNodeType::THREAD_EXIT;
        } else if (m_thread_api->isTDCondWait(&inst)) {
          node_type = SyncNodeType::COND_WAIT;
        } else if (m_thread_api->isTDCondSignal(&inst)) {
          node_type = SyncNodeType::COND_SIGNAL;
        } else if (m_thread_api->isTDCondBroadcast(&inst)) {
          node_type = SyncNodeType::COND_BROADCAST;
        } else if (m_thread_api->isTDBarWait(&inst)) {
          node_type = SyncNodeType::BARRIER_WAIT;
        }
      }
      m_tfg->createNode(&inst, node_type, tid);
    }
  }

  // --- Pass 2: Add edges and handle synchronization logic ---
  SyncNode *entry_node = nullptr;
  if (!func->empty() && !func->front().empty()) {
    entry_node = m_tfg->getNode(&func->front().front());
    if (entry_node) {
      m_tfg->setThreadEntryNode(tid, entry_node);
    }
  }

  SyncNode *exit_node = nullptr;

  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      SyncNode *node = m_tfg->getNode(&inst);
      if (!node) {
        continue;
      }

      exit_node = node;

      if (&inst != &bb.front()) {
        const Instruction *prev_inst = inst.getPrevNode();
        if (prev_inst) {
          SyncNode *prev_node = m_tfg->getNode(prev_inst);
          if (prev_node) {
            m_tfg->addIntraThreadEdge(prev_node, node);
          }
        }
      }

      if (inst.isTerminator()) {
        for (const BasicBlock *succ : successors(inst.getParent())) {
          if (!succ->empty()) {
            SyncNode *succ_node = m_tfg->getNode(&succ->front());
            if (succ_node) {
              m_tfg->addIntraThreadEdge(node, succ_node);
            }
          }
        }
      }

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        if (m_thread_api->isTDFork(&inst)) {
          handleThreadFork(&inst, node, tid);
        } else if (m_thread_api->isTDJoin(&inst)) {
          handleThreadJoin(&inst, node, tid);
        } else if (m_thread_api->isTDAcquire(&inst)) {
          handleLockAcquire(&inst, node);
        } else if (m_thread_api->isTDRelease(&inst)) {
          handleLockRelease(&inst, node);
        } else if (m_thread_api->isTDCondWait(&inst)) {
          handleCondWait(&inst, node);
        } else if (m_thread_api->isTDCondSignal(&inst)) {
          handleCondSignal(&inst, node);
        } else if (m_thread_api->isTDCondBroadcast(&inst)) {
          handleCondSignal(&inst, node);
        } else if (m_thread_api->isTDBarWait(&inst)) {
          handleBarrier(&inst, node);
        } else {
          const Function *callee = cb->getCalledFunction();
          if (callee && !callee->isDeclaration()) {
            processFunction(callee, tid);
          }
        }
      }
    }
  }

  if (exit_node && !m_tfg->getThreadExitNode(tid)) {
    m_tfg->setThreadExitNode(tid, exit_node);
  }
}

void StaticVectorClockMHP::mapInstructionToThread(const Instruction *inst,
                                                  ThreadID tid) {
  if (!inst) {
    return;
  }

  if (m_has_unresolved_fork &&
      m_thread_entry_candidates.find(inst->getFunction()) !=
          m_thread_entry_candidates.end()) {
    m_inst_to_thread[inst] = kUnknownThread;
    return;
  }

  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    m_inst_to_thread[inst] = tid;
    return;
  }

  if (it->second == kUnknownThread || it->second == tid) {
    return;
  }

  it->second = kUnknownThread;
}

bool StaticVectorClockMHP::isInstructionThreadAmbiguous(
    const Instruction *inst) const {
  if (!inst) {
    return true;
  }
  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    return true;
  }
  return it->second == kUnknownThread;
}

bool StaticVectorClockMHP::isMustIntraThreadEdge(const SyncNode *from,
                                                 const SyncNode *to) const {
  if (!from || !to) {
    return false;
  }
  if (from->getThreadID() != to->getThreadID()) {
    return false;
  }

  const Instruction *from_inst = from->getInstruction();
  const Instruction *to_inst = to->getInstruction();
  if (!from_inst || !to_inst) {
    return false;
  }

  if (from_inst->getParent() == to_inst->getParent()) {
    return from_inst->getNextNode() == to_inst;
  }

  const Function *func = from_inst->getFunction();
  if (!func || func != to_inst->getFunction()) {
    return false;
  }

  const PostDominatorTree &PDT = getPostDomTree(func);
  return PDT.dominates(to_inst->getParent(), from_inst->getParent());
}

const PostDominatorTree &
StaticVectorClockMHP::getPostDomTree(const Function *func) const {
  auto it = m_post_dom_cache.find(func);
  if (it != m_post_dom_cache.end()) {
    return *(it->second);
  }
  auto PDT = std::make_unique<PostDominatorTree>();
  PDT->recalculate(*const_cast<Function *>(func));
  auto *pdtPtr = PDT.get();
  m_post_dom_cache[func] = std::move(PDT);
  return *pdtPtr;
}

void StaticVectorClockMHP::enableIndirectForkConservatism() {
  if (m_has_unresolved_fork) {
    return;
  }
  m_has_unresolved_fork = true;

  for (Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    if (!func.hasAddressTaken()) {
      continue;
    }

    m_thread_entry_candidates.insert(&func);
    for (Instruction &inst : instructions(func)) {
      auto it = m_inst_to_thread.find(&inst);
      if (it == m_inst_to_thread.end()) {
        m_inst_to_thread[&inst] = kUnknownThread;
      } else if (it->second != kUnknownThread) {
        it->second = kUnknownThread;
      }
    }
  }
}

ThreadID StaticVectorClockMHP::allocateThreadID() { return m_next_thread_id++; }

void StaticVectorClockMHP::handleThreadFork(const Instruction *fork_inst,
                                            SyncNode *node,
                                            ThreadID parent_tid) {
  ThreadID new_tid = allocateThreadID();

  node->setForkedThread(new_tid);

  m_thread_fork_sites[new_tid] = fork_inst;
  m_thread_parents[new_tid] = parent_tid;
  m_thread_children[parent_tid].push_back(new_tid);

  const Value *pthread_ptr = m_thread_api->getForkedThread(fork_inst);
  if (pthread_ptr) {
    m_pthread_value_to_thread[pthread_ptr] = new_tid;
    m_thread_to_pthread_value[new_tid] = pthread_ptr;
  }

  const Value *forked_fun_val = m_thread_api->getForkedFun(fork_inst);
  if (const Function *forked_fun = dyn_cast_or_null<Function>(forked_fun_val)) {
    m_tfg->addThread(new_tid, forked_fun);
    processFunction(forked_fun, new_tid);
    if (SyncNode *child_entry = m_tfg->getThreadEntryNode(new_tid)) {
      m_tfg->addInterThreadEdge(node, child_entry);
    }
  } else {
    enableIndirectForkConservatism();
  }
}

void StaticVectorClockMHP::handleThreadJoin(const Instruction *join_inst,
                                            SyncNode *node,
                                            ThreadID parent_tid) {
  const Value *joined_thread_val = m_thread_api->getJoinedThread(join_inst);
  ThreadID joined_tid = 0;
  bool found = false;

  if (joined_thread_val) {
    auto it = m_pthread_value_to_thread.find(joined_thread_val);
    if (it != m_pthread_value_to_thread.end()) {
      joined_tid = it->second;
      found = true;
    } else if (const LoadInst *load = dyn_cast<LoadInst>(joined_thread_val)) {
      const Value *loaded_from = load->getPointerOperand();
      auto it2 = m_pthread_value_to_thread.find(loaded_from);
      if (it2 != m_pthread_value_to_thread.end()) {
        joined_tid = it2->second;
        found = true;
        m_pthread_value_to_thread[joined_thread_val] = joined_tid;
      }
    }
  }

  auto add_edge_to_join = [&](ThreadID tid) {
    if (SyncNode *child_exit = m_tfg->getThreadExitNode(tid)) {
      m_tfg->addInterThreadEdge(child_exit, node);
      node->setJoinedThread(tid);
      m_join_to_thread[join_inst] = tid;
    }
  };

  if (found && joined_tid != 0) {
    add_edge_to_join(joined_tid);
  } else {
    (void)parent_tid;
    // Unknown join target: skip adding HB edges to avoid unsound ordering.
  }
}

void StaticVectorClockMHP::handleLockAcquire(const Instruction *lock_inst,
                                             SyncNode *node) {
  const Value *lock = m_thread_api->getLockVal(lock_inst);
  node->setLockValue(lock);
}

void StaticVectorClockMHP::handleLockRelease(const Instruction *unlock_inst,
                                             SyncNode *node) {
  const Value *lock = m_thread_api->getLockVal(unlock_inst);
  node->setLockValue(lock);
}

void StaticVectorClockMHP::handleCondWait(const Instruction *wait_inst,
                                          SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(wait_inst);
  const Value *mutex = m_thread_api->getCondMutex(wait_inst);
  node->setCondValue(cond);
  node->setLockValue(mutex);
  m_condvar_waits[cond].push_back(wait_inst);
}

void StaticVectorClockMHP::handleCondSignal(const Instruction *signal_inst,
                                            SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(signal_inst);
  node->setCondValue(cond);
  m_condvar_signals[cond].push_back(signal_inst);
}

void StaticVectorClockMHP::handleBarrier(const Instruction *barrier_inst,
                                         SyncNode *node) {
  const Value *barrier = m_thread_api->getBarrierVal(barrier_inst);
  node->setLockValue(barrier);
  m_barrier_waits[barrier].push_back(barrier_inst);
}
