/**
 * @file MHPAnalysis.cpp
 * @brief Implementation of May-Happen-in-Parallel Analysis
 *
 * This analysis constructs a Thread Flow Graph (TFG) to determine which
 * instructions may execute concurrently.
 *
 * Soundness Properties:
 * - Default Safety: The analysis is a conservative overlap oracle.
 *   It assumes two instructions may be concurrent unless structural
 *   non-overlap is proven.
 * - Synchronization:
 *   - Fork/Join: Precisely models ancestor relationships.
 *   - Condition Variables: Treats wait/signal/broadcast as synchronization
 *     boundaries, but does not synthesize inter-thread ordering.
 *   - Barriers/OpenMP waits: Enforces structural non-overlap when the runtime
 *     primitive guarantees it.
 * - Thread Instances: Conservatively assumes spawned threads may have multiple
 * instances.
 *
 * Author: rainoftime
 */

#include "Analysis/Concurrency/MHP/MHPAnalysis.h"

#include <deque>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

// ============================================================================
// ThreadRegionAnalysis Implementation
// ============================================================================

void ThreadRegionAnalysis::analyze() {
  identifyRegions();
  computeOrderingConstraints();
  computeParallelism();
}

const ThreadRegionAnalysis::Region *
ThreadRegionAnalysis::getRegion(size_t region_id) const {
  if (region_id < m_regions.size()) {
    return m_regions[region_id].get();
  }
  return nullptr;
}

const ThreadRegionAnalysis::Region *
ThreadRegionAnalysis::getRegionContaining(const Instruction *inst) const {
  auto it = m_inst_to_region.find(inst);
  return it != m_inst_to_region.end() ? it->second : nullptr;
}

// ============================================================================
// CFG-based Region Analysis Helpers
// ============================================================================

bool ThreadRegionAnalysis::isSyncPoint(const Instruction *inst) const {
  for (SyncNode *node : m_tfg.getNodes(inst)) {
    SyncNodeType type = node->getType();
    if (isSynchronizationNode(type) || isThreadBoundaryNode(type)) {
      return true;
    }
  }
  return false;
}

std::vector<const Instruction *>
ThreadRegionAnalysis::collectSyncPoints(const Function *func) const {
  std::vector<const Instruction *> sync_points;

  for (const BasicBlock &BB : *func) {
    for (const Instruction &inst : BB) {
      if (isSyncPoint(&inst)) {
        sync_points.push_back(&inst);
      }
    }
  }

  return sync_points;
}

void ThreadRegionAnalysis::identifyRegions() {
  auto threads = m_tfg.getAllThreads();

  for (ThreadID tid : threads) {
    const Function *entry = m_tfg.getThreadEntry(tid);
    if (entry && !entry->isDeclaration()) {
      identifyRegionsForThread(tid, entry);
    }
  }
}

void ThreadRegionAnalysis::identifyRegionsForThread(ThreadID tid,
                                                    const Function *func) {
  if (!func || func->isDeclaration())
    return;

  auto flush_region = [&](std::unique_ptr<Region> &region) {
    if (!region || region->instructions.empty()) {
      return;
    }
    for (const Instruction *inst : region->instructions) {
      m_inst_to_region[inst] = region.get();
    }
    m_regions.push_back(std::move(region));
  };

  auto make_region = [&](SyncNode *start_node) {
    auto region = std::make_unique<Region>();
    region->region_id = m_regions.size();
    region->thread_id = tid;
    region->start_node = start_node;
    region->end_node = nullptr;
    return region;
  };

  for (const BasicBlock &BB : *func) {
    SyncNode *pending_start = nullptr;
    std::unique_ptr<Region> region;

    for (const Instruction &inst : BB) {
      if (!region) {
        region = make_region(pending_start);
        pending_start = nullptr;
      }

      region->instructions.insert(&inst);

      if (!isSyncPoint(&inst)) {
        continue;
      }

      region->end_node = m_tfg.getNode(&inst, tid);
      flush_region(region);
      pending_start = m_tfg.getNode(&inst, tid);
    }

    if (region) {
      flush_region(region);
    }
  }
}

void ThreadRegionAnalysis::computeOrderingConstraints() {
  // Compute must-precede and must-follow relationships based on:
  // 1. Intra-thread control flow
  // 2. Fork-join relationships
  // 3. Lock ordering

  for (const auto &region : m_regions) {
    region->must_precede_bits.resize(m_regions.size());
    region->must_follow_bits.resize(m_regions.size());
    region->may_be_parallel_bits.resize(m_regions.size());
  }

  for (size_t i = 0; i < m_regions.size(); ++i) {
    Region &region_i = *m_regions[i];

    for (size_t j = 0; j < m_regions.size(); ++j) {
      if (i == j)
        continue;

      Region &region_j = *m_regions[j];

      // Same thread: use CFG ordering via sync nodes
      if (region_i.thread_id == region_j.thread_id) {
        // Check if region_i must precede region_j via sync node ordering
        if (region_i.end_node && region_j.start_node) {
          // Use TFG reachability to check if end_i reaches start_j
          if (isReachableInTFG(region_i.end_node, region_j.start_node)) {
            region_i.must_precede.insert(j);
            region_j.must_follow.insert(i);
          }
        }
        continue;
      }

      // Different threads: check fork-join synchronization

      // Case 1: Fork ordering
      // If region_i contains a fork creating thread_j, then region_i precedes
      // the entry region of thread_j
      if (region_i.end_node) {
        SyncNodeType type_i = region_i.end_node->getType();
        if (type_i == SyncNodeType::THREAD_FORK) {
          ThreadID forked_tid = region_i.end_node->getForkedThread();
          if (forked_tid == region_j.thread_id) {
            // Fork creates thread j: region i must precede region j
            region_i.must_precede.insert(j);
            region_j.must_follow.insert(i);
          }
        }
      }

      // Case 2: Join ordering
      // If region_i contains a join for thread_j, then the exit region of
      // thread_j must precede region_i
      if (region_i.start_node) {
        SyncNodeType type_i = region_i.start_node->getType();
        if (type_i == SyncNodeType::THREAD_JOIN) {
          ThreadID joined_tid = region_i.start_node->getJoinedThread();
          if (joined_tid == region_j.thread_id) {
            // Join waits for thread j: region j must precede region i
            region_j.must_precede.insert(i);
            region_i.must_follow.insert(j);
          }
        }
      }

      // Note: Lock-based ordering is complex and would require tracking
      // which lock acquisition happens first in the global execution.
      // This would need a more sophisticated lock order analysis.
      // For now, we rely on the LockSetAnalysis to identify conflicts.
    }
  }
}

bool ThreadRegionAnalysis::isReachableInTFG(const SyncNode *from,
                                            const SyncNode *to) const {
  if (!from || !to || from == to) {
    return false;
  }

  // Simple BFS reachability check in the TFG
  std::deque<const SyncNode *> worklist;
  std::unordered_set<const SyncNode *> visited;

  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == to) {
      return true;
    }

    // Only follow intra-thread edges for same-thread reachability
    if (current->getThreadID() == from->getThreadID()) {
      for (SyncNode *succ : current->getSuccessors()) {
        if (succ->getThreadID() == from->getThreadID() &&
            visited.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
  }

  return false;
}

void ThreadRegionAnalysis::computeParallelism() {
  // Two regions may run in parallel if:
  // 1. They are in different threads
  // 2. Neither must precede the other
  // 3. They don't hold conflicting locks

  for (size_t i = 0; i < m_regions.size(); ++i) {
    auto &region_i = m_regions[i];

    // Initialize bitvectors from sets
    for (size_t j : region_i->must_precede) {
      region_i->must_precede_bits.set(j);
    }
    for (size_t j : region_i->must_follow) {
      region_i->must_follow_bits.set(j);
    }

    for (size_t j = i + 1; j < m_regions.size(); ++j) {
      auto &region_j = m_regions[j];

      // Same thread => not parallel
      if (region_i->thread_id == region_j->thread_id) {
        continue;
      }

      // Check ordering constraints
      if (region_i->must_precede.find(j) != region_i->must_precede.end() ||
          region_i->must_follow.find(j) != region_i->must_follow.end()) {
        continue;
      }

      // May be parallel - set both set and bitvector
      region_i->may_be_parallel.insert(j);
      region_j->may_be_parallel.insert(i);
      region_i->may_be_parallel_bits.set(j);
      region_j->may_be_parallel_bits.set(i);
    }
  }
}

void ThreadRegionAnalysis::print(raw_ostream &os) const {
  os << "Thread Region Analysis Results:\n";
  os << "================================\n";
  os << "Total Regions: " << m_regions.size() << "\n\n";

  for (const auto &region : m_regions) {
    os << "Region " << region->region_id << " (Thread " << region->thread_id
       << "):\n";
    os << "  Instructions: " << region->instructions.size() << "\n";
    os << "  Must Precede: {";
    bool first = true;
    for (auto r : region->must_precede) {
      if (!first)
        os << ", ";
      os << r;
      first = false;
    }
    os << "}\n";

    os << "  May Be Parallel: {";
    first = true;
    for (auto r : region->may_be_parallel) {
      if (!first)
        os << ", ";
      os << r;
      first = false;
    }
    os << "}\n\n";
  }
}

// ============================================================================
// MHPAnalysis Implementation
// ============================================================================

MHPAnalysis::MHPAnalysis(Module &module)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()) {
  m_tfg = std::make_unique<ThreadFlowGraph>();
  m_alias_analysis = lotus::AliasAnalysisFactory::create(
      m_module, lotus::AAConfig::SparrowAA_NoCtx());
  m_call_graph = std::make_unique<CallGraph>(m_module);
}

MHPAnalysis::~MHPAnalysis() = default;

void MHPAnalysis::analyze() {
  errs() << "Starting MHP Analysis...\n";

  m_order_cache.clear();
  m_mhp_cache.clear();
  m_mhp_pairs.clear();
  m_inst_to_thread.clear();
  m_thread_fork_sites.clear();
  m_thread_parents.clear();
  m_thread_children.clear();
  m_fork_to_thread.clear();
  m_join_to_thread.clear();
  m_pthread_value_to_threads.clear();
  m_thread_to_pthread_value.clear();
  m_condvar_signals.clear();
  m_condvar_waits.clear();
  m_barrier_waits.clear();
  m_barrier_phase_by_thread.clear();
  m_visited_functions_by_thread.clear();
  m_pre_fork_main_nodes.clear();
  m_thread_entry_candidates.clear();
  m_has_unresolved_fork = false;
  m_has_multi_context_nodes = false;
  m_multi_instance_threads.clear();
  m_dom_cache.clear();
  m_openmp_task_threads.clear();
  m_openmp_task_exclusions.clear();
  m_openmp_semantics = std::make_unique<OpenMP::OpenMPSemantics>(m_module);
  m_openmp_semantics->analyze();
  m_next_thread_id = 1;
  m_region_analysis.reset();
  m_lockset.reset();
  m_tfg = std::make_unique<ThreadFlowGraph>();
  m_call_graph = std::make_unique<CallGraph>(m_module);
  m_join_target_analysis =
      std::make_unique<JoinTargetAnalysis>(m_module, m_alias_analysis.get());

  buildThreadFlowGraph();

  // Optional: LockSet analysis for more precise reasoning
  if (m_enable_lockset_analysis) {
    analyzeLockSets();
  }

  analyzeThreadRegions();
  if (m_precompute_mhp_pairs) {
    computeMHPPairs();
  }

  errs() << "MHP Analysis Complete!\n";
}

void MHPAnalysis::enableLockSetAnalysis() { m_enable_lockset_analysis = true; }

void MHPAnalysis::buildThreadFlowGraph() {
  errs() << "Building Thread Flow Graph...\n";

  // Find main function
  Function *main_func = m_module.getFunction("main");
  if (!main_func) {
    errs() << "Warning: No main function found\n";
    return;
  }

  if (m_join_target_analysis) {
    m_join_target_analysis->analyze();
  }

  // Main thread (thread 0)
  m_tfg->addThread(0, main_func);
  processFunction(main_func, 0, 0);
  finalizeBarrierPhases();
  if (m_openmp_semantics) {
    lowerOpenMPTasks(*m_openmp_semantics);
  }

  // Build reachability index for faster structural-order queries
  m_tfg->buildReachabilityIndex();
  recomputePreForkMainNodes();

  errs() << "Thread Flow Graph built with " << m_tfg->getAllNodes().size()
         << " nodes\n";
}

std::pair<ThreadID, ThreadID>
MHPAnalysis::normalizeThreadPair(ThreadID lhs, ThreadID rhs) const {
  return lhs < rhs ? std::make_pair(lhs, rhs) : std::make_pair(rhs, lhs);
}

void MHPAnalysis::lowerOpenMPTasks(const OpenMP::OpenMPSemantics &semantics) {
  auto wireInlineTask = [&](const OpenMP::Task *task, ThreadID parent_tid) {
    SyncNode *create_node = m_tfg->getNode(task->task_create, parent_tid);
    if (!create_node) {
      return;
    }

    CallContextID callee_ctx = create_node->getNodeID();
    m_has_multi_context_nodes = true;
    processFunction(task->task_function, parent_tid, callee_ctx);

    if (SyncNode *task_entry = m_tfg->getNode(
            &task->task_function->front().front(), parent_tid, callee_ctx)) {
      m_tfg->addCallEdge(create_node, task_entry);
    }

    SyncNode *task_exit =
        m_tfg->getFunctionExitNode(parent_tid, task->task_function, callee_ctx);
    if (!task_exit) {
      return;
    }

    if (const Instruction *next_inst = task->task_create->getNextNode()) {
      if (SyncNode *return_site = m_tfg->getNode(next_inst, parent_tid)) {
        m_tfg->addRetEdge(task_exit, return_site);
      }
      return;
    }

    if (task->task_create->isTerminator()) {
      for (const BasicBlock *succ :
           successors(task->task_create->getParent())) {
        if (succ->empty()) {
          continue;
        }
        if (SyncNode *return_site =
                m_tfg->getNode(&succ->front(), parent_tid)) {
          m_tfg->addRetEdge(task_exit, return_site);
        }
      }
    }
  };

  for (const auto &task_uptr : semantics.getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    if (!task || !task->task_create || !task->task_function ||
        task->task_function->isDeclaration()) {
      continue;
    }

    ThreadID parent_tid = getThreadID(task->task_create);
    if (parent_tid == kUnknownThread) {
      continue;
    }

    if (task->execution_mode == OpenMP::TaskExecutionMode::Included) {
      wireInlineTask(task, parent_tid);
      continue;
    }

    ThreadID task_tid = allocateThreadID();
    m_openmp_task_threads[task->task_create] = task_tid;
    m_thread_fork_sites[task_tid] = task->task_create;
    m_thread_parents[task_tid] = parent_tid;
    m_thread_children[parent_tid].push_back(task_tid);
    m_fork_to_thread[task->task_create] = task_tid;

    m_tfg->addThread(task_tid, task->task_function);
    processFunction(task->task_function, task_tid, 0);

    if (SyncNode *create_node = m_tfg->getNode(task->task_create, parent_tid)) {
      create_node->setForkedThread(task_tid);
      if (SyncNode *task_entry = m_tfg->getThreadEntryNode(task_tid)) {
        m_tfg->addInterThreadEdge(create_node, task_entry);
      }
    }
  }

  auto getTaskThread = [&](const OpenMP::Task *task) -> ThreadID {
    if (!task || !task->task_create) {
      return 0;
    }
    auto it = m_openmp_task_threads.find(task->task_create);
    return it != m_openmp_task_threads.end() ? it->second : 0;
  };

  for (const auto &task_uptr : semantics.getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    ThreadID task_tid = getTaskThread(task);
    if (!task_tid) {
      continue;
    }

    SyncNode *task_entry = m_tfg->getThreadEntryNode(task_tid);
    for (const OpenMP::Task *pred : task->predecessors) {
      ThreadID pred_tid = getTaskThread(pred);
      SyncNode *pred_exit =
          pred_tid ? m_tfg->getThreadExitNode(pred_tid) : nullptr;
      if (pred_exit && task_entry) {
        m_tfg->addInterThreadEdge(pred_exit, task_entry);
      }
    }

    for (const OpenMP::Task *excluded : task->exclusions) {
      ThreadID excluded_tid = getTaskThread(excluded);
      if (excluded_tid) {
        m_openmp_task_exclusions.insert(
            normalizeThreadPair(task_tid, excluded_tid));
      }
    }
  }

  for (const auto &task_uptr : semantics.getTasks()) {
    const OpenMP::Task *task = task_uptr.get();
    ThreadID task_tid = getTaskThread(task);
    if (!task_tid) {
      continue;
    }
    for (const OpenMP::Task *excluded : task->exclusions) {
      ThreadID excluded_tid = getTaskThread(excluded);
      if (!excluded_tid || task_tid == excluded_tid) {
        continue;
      }
      m_openmp_task_exclusions.insert(
          normalizeThreadPair(task_tid, excluded_tid));
    }
  }

  for (const auto &boundary : semantics.getWaitBoundaryInfos()) {
    if (!boundary.inst) {
      continue;
    }
    if (boundary.is_partial_wait) {
      continue;
    }

    ThreadID parent_tid = getThreadID(boundary.inst);
    if (parent_tid == kUnknownThread) {
      continue;
    }

    SyncNode *wait_node = m_tfg->getNode(boundary.inst, parent_tid);
    if (!wait_node) {
      continue;
    }

    for (const auto &task_uptr : semantics.getTasks()) {
      const OpenMP::Task *task = task_uptr.get();
      if (!task ||
          task->scheduling_context_id != boundary.scheduling_context_id ||
          task->sequence_index >= boundary.sequence_index) {
        continue;
      }
      if (boundary.is_taskgroup_end) {
        if (boundary.taskgroup_id == 0 ||
            task->taskgroup_id != boundary.taskgroup_id) {
          continue;
        }
      } else if (task->phase_id != boundary.phase_id) {
        continue;
      }

      ThreadID task_tid = getTaskThread(task);
      SyncNode *task_exit =
          task_tid ? m_tfg->getThreadExitNode(task_tid) : nullptr;
      if (task_exit) {
        m_tfg->addInterThreadEdge(task_exit, wait_node);
      }
    }
  }
}

void MHPAnalysis::processFunction(const Function *func, ThreadID tid,
                                  CallContextID ctx) {
  if (!func || func->isDeclaration())
    return;

  // Avoid re-processing functions for the same thread context
  if (m_visited_functions_by_thread[tid][ctx].count(func)) {
    return;
  }
  m_visited_functions_by_thread[tid][ctx].insert(func);

  // --- Pass 1: Create all nodes for this function ---
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      mapInstructionToThread(&inst, tid);
      SyncNodeType node_type = SyncNodeType::REGULAR_INST;

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
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
        } else if (m_thread_api->isTDCondBroadcast(&inst)) {
          node_type = SyncNodeType::COND_BROADCAST;
        } else if (m_thread_api->isTDCondSignal(&inst)) {
          node_type = SyncNodeType::COND_SIGNAL;
        } else if (type == ThreadAPI::TD_LATCH_ARRIVE_WAIT ||
                   m_thread_api->isTDBarWait(&inst)) {
          node_type = SyncNodeType::BARRIER_WAIT;
        }
      }
      m_tfg->createNode(&inst, node_type, tid, ctx);
    }
  }

  // --- Pass 2: Add edges and handle synchronization logic ---
  // Set entry node to first instruction in entry block
  SyncNode *entry_node = nullptr;
  if (!func->empty() && !func->front().empty()) {
    entry_node = m_tfg->getNode(&func->front().front(), tid, ctx);
    if (entry_node && ctx == 0) {
      m_tfg->setThreadEntryNode(tid, entry_node);
    }
  }

  SyncNode *exit_node = nullptr;

  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      SyncNode *node = m_tfg->getNode(&inst, tid, ctx);
      if (!node)
        continue;

      // Update exit node to last instruction we see
      exit_node = node;

      // Add intra-block edges
      if (&inst != &bb.front()) {
        const Instruction *prev_inst = inst.getPrevNode();
        if (prev_inst) {
          SyncNode *prev_node = m_tfg->getNode(prev_inst, tid, ctx);
          if (prev_node)
            m_tfg->addIntraThreadEdge(prev_node, node);
        }
      }

      // Add inter-block (CFG) edges
      if (inst.isTerminator()) {
        for (const BasicBlock *succ : successors(inst.getParent())) {
          if (!succ->empty()) {
            SyncNode *succ_node = m_tfg->getNode(&succ->front(), tid, ctx);
            if (succ_node)
              m_tfg->addIntraThreadEdge(node, succ_node);
          }
        }
      }

      // Handle synchronization logic for special instructions
      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
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
        } else if (m_thread_api->isTDCondSignal(&inst) ||
                   m_thread_api->isTDCondBroadcast(&inst)) {
          handleCondSignal(&inst, node);
        } else if (type == ThreadAPI::TD_LATCH_ARRIVE_WAIT ||
                   m_thread_api->isTDBarWait(&inst)) {
          handleBarrier(&inst, node);
        } else {
          // Handle both direct and indirect calls
          const Function *callee = m_thread_api->getCallee(cb);
          auto wireDirectCall = [&](const Function *target) {
            if (!target || target->isDeclaration()) {
              return;
            }
            CallContextID callee_ctx = node->getNodeID();
            m_has_multi_context_nodes = true;
            processFunction(target, tid, callee_ctx);
            SyncNode *callee_entry =
                m_tfg->getNode(&target->front().front(), tid, callee_ctx);
            if (callee_entry) {
              m_tfg->addCallEdge(node, callee_entry);
            }
            SyncNode *callee_exit =
                m_tfg->getFunctionExitNode(tid, target, callee_ctx);
            if (!callee_exit) {
              return;
            }
            const Instruction *next_inst = inst.getNextNode();
            if (next_inst) {
              if (SyncNode *return_site = m_tfg->getNode(next_inst, tid, ctx)) {
                m_tfg->addRetEdge(callee_exit, return_site);
              }
            } else if (inst.isTerminator()) {
              for (const BasicBlock *succ : successors(inst.getParent())) {
                if (succ->empty()) {
                  continue;
                }
                if (SyncNode *return_site =
                        m_tfg->getNode(&succ->front(), tid, ctx)) {
                  m_tfg->addRetEdge(callee_exit, return_site);
                }
              }
            }
          };

          if (m_thread_api->getType(cb) == ThreadAPI::TD_CALL_ONCE) {
            for (unsigned idx = 1; idx < cb->arg_size(); ++idx) {
              const Value *arg = cb->getArgOperand(idx);
              if (!arg) {
                continue;
              }
              if (const auto *callable =
                      dyn_cast<Function>(arg->stripPointerCasts())) {
                wireDirectCall(callable);
                break;
              }
            }
          }

          if (!callee) {
            bool resolved_indirect_target = false;
            // Indirect call: use call graph to find possible callees
            if (m_call_graph) {
              CallGraphNode *cgNode = (*m_call_graph)[cb->getFunction()];
              if (cgNode) {
                // Find the call record for this call site
                for (auto &callRecord : *cgNode) {
                  if (callRecord.first.hasValue() &&
                      dyn_cast_or_null<CallBase>(*callRecord.first) == cb) {
                    CallGraphNode *calleeNode = callRecord.second;
                    if (calleeNode) {
                      Function *possibleCallee = calleeNode->getFunction();
                      if (possibleCallee && !possibleCallee->isDeclaration()) {
                        resolved_indirect_target = true;
                        CallContextID callee_ctx = node->getNodeID();
                        m_has_multi_context_nodes = true;
                        // Process this possible callee
                        processFunction(possibleCallee, tid, callee_ctx);
                        SyncNode *callee_entry = m_tfg->getNode(
                            &possibleCallee->front().front(), tid, callee_ctx);
                        if (callee_entry) {
                          m_tfg->addCallEdge(node, callee_entry);
                        }
                        SyncNode *callee_exit = m_tfg->getFunctionExitNode(
                            tid, possibleCallee, callee_ctx);
                        if (callee_exit) {
                          const Instruction *next_inst = inst.getNextNode();
                          if (next_inst) {
                            if (SyncNode *return_site =
                                    m_tfg->getNode(next_inst, tid, ctx)) {
                              m_tfg->addRetEdge(callee_exit, return_site);
                            }
                          } else if (inst.isTerminator()) {
                            for (const BasicBlock *succ :
                                 successors(inst.getParent())) {
                              if (succ->empty()) {
                                continue;
                              }
                              if (SyncNode *return_site = m_tfg->getNode(
                                      &succ->front(), tid, ctx)) {
                                m_tfg->addRetEdge(callee_exit, return_site);
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
            if (!resolved_indirect_target) {
              // Conservatively assume an unresolved indirect call could hide a
              // thread entry and avoid proving non-overlap through optimistic
              // single-thread reasoning.
              enableIndirectForkConservatism();
            }
          } else if (!callee->isDeclaration()) {
            // Direct call to a defined function
            wireDirectCall(callee);
          }
        }
      }
    }
  }

  // Set exit node if we haven't set it yet
  if (exit_node && !m_tfg->getThreadExitNode(tid)) {
    m_tfg->setThreadExitNode(tid, exit_node);
  }
  if (exit_node) {
    m_tfg->setFunctionExitNode(tid, func, exit_node, ctx);
  }
}

void MHPAnalysis::processInstruction(const Instruction * /*inst*/,
                                     ThreadID /*tid*/,
                                     SyncNode *& /*current_node*/) {
  // This method is a helper for more fine-grained processing if needed
  // Currently unused but kept for future extensibility
}

void MHPAnalysis::handleThreadFork(const Instruction *fork_inst, SyncNode *node,
                                   ThreadID parent_tid) {
  // Allocate new thread ID
  ThreadID new_tid = allocateThreadID();

  // Mark as multi-instance only when the fork site is in a loop.
  // This reduces false positives for common "fork once" patterns.
  bool in_loop = false;
  if (fork_inst && fork_inst->getFunction()) {
    const Function *F = fork_inst->getFunction();
    DominatorTree &DT = const_cast<DominatorTree &>(getDomTree(F));
    LoopInfo LI;
    LI.analyze(DT);
    in_loop = (LI.getLoopFor(fork_inst->getParent()) != nullptr);
  }
  if (in_loop) {
    m_multi_instance_threads.insert(new_tid);
  }

  node->setForkedThread(new_tid);

  // Track fork-join relationships
  m_thread_fork_sites[new_tid] = fork_inst;
  m_thread_parents[new_tid] = parent_tid;
  m_thread_children[parent_tid].push_back(new_tid);
  m_fork_to_thread[fork_inst] = new_tid;

  // Track pthread_t value for this thread
  // The first argument to pthread_create is the pthread_t* where the thread ID
  // is stored
  const Value *pthread_ptr = m_thread_api->getForkedThread(fork_inst);
  if (pthread_ptr) {
    // Map this pthread_t pointer to the thread ID
    // We need to track both the pointer and any loads from it
    m_pthread_value_to_threads[pthread_ptr].insert(new_tid);
    m_thread_to_pthread_value[new_tid] = pthread_ptr;

    // Also track the store if it exists (for later load tracking)
    // In a more sophisticated implementation, we'd do def-use chain analysis
  }

  // Get the forked function
  const Value *forked_fun_val = m_thread_api->getForkedFun(fork_inst);
  if (const Function *forked_fun = dyn_cast_or_null<Function>(forked_fun_val)) {
    m_tfg->addThread(new_tid, forked_fun);

    // Process the forked function
    processFunction(forked_fun, new_tid, 0);

    // Add inter-thread edge from fork to new thread entry
    SyncNode *new_thread_entry = m_tfg->getThreadEntryNode(new_tid);
    if (new_thread_entry) {
      m_tfg->addInterThreadEdge(node, new_thread_entry);
    }
  } else {
    enableIndirectForkConservatism();
  }
}

void MHPAnalysis::enableIndirectForkConservatism() {
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

#include <deque>
#include <set>

// ============================================================================
// Value Tracing Helpers
// ============================================================================
const Value *MHPAnalysis::tracePthreadT(const Value *val) const {
  return JoinTargetAnalysis::traceThreadHandleRoot(val, &m_module);
}

void MHPAnalysis::handleThreadJoin(const Instruction *join_inst, SyncNode *node,
                                   ThreadID parent_tid) {
  // Track which thread is being joined using value analysis
  // pthread_join takes the pthread_t value (not pointer) as first argument

  const Value *joined_thread_val = m_thread_api->getJoinedThread(join_inst);
  ThreadID joined_tid = 0;
  bool found_thread = false;
  std::unordered_set<const Value *> joined_roots;

  if (joined_thread_val) {
    JoinTargetAnalysis::traceThreadHandleRoots(joined_thread_val, &m_module,
                                               joined_roots);
    // Use the improved tracing function to find the origin of the pthread_t
    // value.
    const Value *pthread_t_origin = tracePthreadT(joined_thread_val);

    if (pthread_t_origin) {
      auto it = m_pthread_value_to_threads.find(pthread_t_origin);
      if (it != m_pthread_value_to_threads.end() && it->second.size() == 1) {
        joined_tid = *it->second.begin();
        found_thread = true;
        // Cache only unambiguous results for the original value.
        if (pthread_t_origin != joined_thread_val) {
          m_pthread_value_to_threads[joined_thread_val] = it->second;
        }
      }
    }
  }

  if (!found_thread && joined_roots.size() <= 1 && m_join_target_analysis) {
    if (m_join_target_analysis->isUnambiguousJoin(join_inst)) {
      std::vector<const Instruction *> possible_forks =
          m_join_target_analysis->getFeasibleJoinedForks(join_inst);
      if (possible_forks.size() == 1) {
        auto it = m_fork_to_thread.find(possible_forks.front());
        if (it != m_fork_to_thread.end()) {
          joined_tid = it->second;
          found_thread = true;
        }
      }
    }
  }

  if (found_thread && joined_tid != 0) {
    // We successfully identified the joined thread
    SyncNode *child_exit = m_tfg->getThreadExitNode(joined_tid);
    if (child_exit) {
      m_tfg->addInterThreadEdge(child_exit, node);
      node->setJoinedThread(joined_tid);
      m_join_to_thread[join_inst] = joined_tid;
    }
  } else {
    // Ambiguous joins must not create structural non-overlap edges.
    (void)parent_tid;
  }
}

void MHPAnalysis::handleLockAcquire(const Instruction *lock_inst,
                                    SyncNode *node) {
  const Value *lock = m_thread_api->getAnalysisLockIdentity(lock_inst);
  node->setLockValue(lock);
}

void MHPAnalysis::handleLockRelease(const Instruction *unlock_inst,
                                    SyncNode *node) {
  const Value *lock = m_thread_api->getAnalysisLockIdentity(unlock_inst);
  node->setLockValue(lock);
}

void MHPAnalysis::handleCondWait(const Instruction *wait_inst, SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(wait_inst);
  const Value *mutex = m_thread_api->getCondMutex(wait_inst);

  node->setCondValue(cond);
  node->setLockValue(mutex);

  // Track the wait as a region boundary only.
  m_condvar_waits[cond].push_back(node);
}

void MHPAnalysis::handleCondSignal(const Instruction *signal_inst,
                                   SyncNode *node) {
  // B2 fix: use getCondVal (not getLockVal) for the condition variable.
  // getLockVal asserts isTDAcquire||isTDRelease and would crash on a signal.
  const Value *cond = m_thread_api->getCondVal(signal_inst);
  node->setCondValue(cond);
  // Record the signal/broadcast as a region boundary only.
  m_condvar_signals[cond].push_back(node);
}

void MHPAnalysis::handleBarrier(const Instruction *barrier_inst,
                                SyncNode *node) {
  const Value *barrier = m_thread_api->getBarrierVal(barrier_inst);
  if (!barrier) {
    barrier = barrier_inst;
  }
  node->setLockValue(barrier); // Reuse lock field for barrier value

  BarrierParticipant current;
  current.arrival = node;
  current.continuations = getBarrierContinuations(barrier_inst);

  size_t phase = m_barrier_phase_by_thread[barrier][node->getThreadID()]++;
  m_barrier_waits[barrier][phase].push_back(std::move(current));
}

void MHPAnalysis::finalizeBarrierPhases() {
  for (const auto &barrier_entry : m_barrier_waits) {
    for (const auto &phase_entry : barrier_entry.second) {
      const std::vector<BarrierParticipant> &participants = phase_entry.second;
      for (size_t i = 0; i < participants.size(); ++i) {
        const BarrierParticipant &lhs = participants[i];
        if (!lhs.arrival) {
          continue;
        }
        for (size_t j = 0; j < participants.size(); ++j) {
          if (i == j) {
            continue;
          }
          const BarrierParticipant &rhs = participants[j];
          if (!rhs.arrival ||
              lhs.arrival->getThreadID() == rhs.arrival->getThreadID()) {
            continue;
          }
          for (SyncNode *cont : rhs.continuations) {
            if (cont) {
              m_tfg->addInterThreadEdge(lhs.arrival, cont);
            }
          }
        }
      }
    }
  }
}

void MHPAnalysis::analyzeLockSets() {
  errs() << "Analyzing Lock Sets...\n";
  m_lockset = std::make_unique<LockSetAnalysis>(m_module);
  m_lockset->setAliasAnalysis(m_alias_analysis.get());
  // B10 fix: use the module-owned CallGraph (m_call_graph) instead of a
  // local CG that would be destroyed at the end of this function, leaving
  // m_lockset with a dangling pointer.
  m_lockset->setCallGraph(m_call_graph.get());
  m_lockset->analyze();
}

void MHPAnalysis::analyzeThreadRegions() {
  errs() << "Analyzing Thread Regions...\n";
  m_region_analysis = std::make_unique<ThreadRegionAnalysis>(*m_tfg);
  m_region_analysis->analyze();
  errs() << "Identified " << m_region_analysis->getAllRegions().size()
         << " regions\n";
}

void MHPAnalysis::computeMHPPairs() {
  errs()
      << "Computing MHP Pairs (Region-Based with Bitvector Optimization)...\n";

  if (!m_region_analysis) {
    errs() << "Warning: Region analysis not available, using instruction-level "
              "computation\n";
    computeMHPPairsInstructionLevel();
    return;
  }

  const auto &regions = m_region_analysis->getAllRegions();
  size_t num_regions = regions.size();
  std::vector<bool> region_prefork_main(num_regions, false);
  for (size_t i = 0; i < num_regions; ++i) {
    const auto &region = regions[i];
    if (region->thread_id != 0 || region->instructions.empty()) {
      continue;
    }
    bool all_prefork = true;
    for (const Instruction *inst : region->instructions) {
      if (!isAlwaysPreForkMain(inst)) {
        all_prefork = false;
        break;
      }
    }
    region_prefork_main[i] = all_prefork;
  }

  // Phase 1: Compute MHP region pairs using bitvector operations
  std::vector<std::pair<size_t, size_t>> mhp_region_pairs;
  mhp_region_pairs.reserve(num_regions * num_regions / 4);

  for (size_t i = 0; i < num_regions; ++i) {
    const auto &region_i = regions[i];
    ThreadID tid_i = region_i->thread_id;

    for (size_t j = i + 1; j < num_regions; ++j) {
      const auto &region_j = regions[j];
      ThreadID tid_j = region_j->thread_id;

      // Same thread (non-multi-instance) cannot be MHP
      if (tid_i == tid_j && !m_multi_instance_threads.count(tid_i)) {
        continue;
      }

      if (m_openmp_task_exclusions.count(normalizeThreadPair(tid_i, tid_j))) {
        continue;
      }

      if ((region_prefork_main[i] && tid_j != 0) ||
          (region_prefork_main[j] && tid_i != 0)) {
        continue;
      }

      // Use bitvector test for O(1) check
      if (region_i->may_be_parallel_bits.test(j)) {
        mhp_region_pairs.push_back({i, j});
      }
    }
  }

  errs() << "Found " << mhp_region_pairs.size() << " MHP region pairs\n";

  // Phase 3: Expand region-level MHP to instruction-level MHP using only
  // structural overlap constraints. Mutual exclusion and memory ordering are
  // checker/HB concerns, not part of pure MHP.
  size_t num_pairs = 0;
  size_t ordered_filtered = 0;

  for (const auto &pair : mhp_region_pairs) {
    size_t ri = pair.first, rj = pair.second;
    const auto &region_i = regions[ri];
    const auto &region_j = regions[rj];
    const Instruction *region_i_end =
        region_i->end_node ? region_i->end_node->getInstruction() : nullptr;
    const Instruction *region_i_start =
        region_i->start_node ? region_i->start_node->getInstruction() : nullptr;
    const Instruction *region_j_end =
        region_j->end_node ? region_j->end_node->getInstruction() : nullptr;
    const Instruction *region_j_start =
        region_j->start_node ? region_j->start_node->getInstruction() : nullptr;

    if ((region_i_end && region_j_start &&
         hasStructuralOrderRelation(region_i_end, region_j_start)) ||
        (region_j_end && region_i_start &&
         hasStructuralOrderRelation(region_j_end, region_i_start))) {
      ordered_filtered += region_i->instructions.size() *
                          region_j->instructions.size();
      continue;
    }

    for (const Instruction *inst_i : region_i->instructions) {
      for (const Instruction *inst_j : region_j->instructions) {
        if (hasStructuralOrderRelation(inst_i, inst_j) ||
            hasStructuralOrderRelation(inst_j, inst_i)) {
          ordered_filtered++;
          continue;
        }

        // Store in canonical (pointer-sorted) order for O(1) symmetric lookup.
        const Instruction *ca = inst_i < inst_j ? inst_i : inst_j;
        const Instruction *cb = inst_i < inst_j ? inst_j : inst_i;
        m_mhp_pairs.insert({ca, cb});
        num_pairs++;
      }
    }
  }

  errs() << "Expanded to " << num_pairs << " MHP instruction pairs "
         << "(filtered " << ordered_filtered << " by structural ordering)\n";
}

void MHPAnalysis::computeMHPPairsInstructionLevel() {
  errs() << "Computing MHP Pairs (Instruction-Level Fallback)...\n";

  std::vector<const Instruction *> all_insts;
  all_insts.reserve(m_inst_to_thread.size());

  for (const auto &entry : m_inst_to_thread) {
    all_insts.push_back(entry.first);
  }

  size_t num_pairs = 0;
  for (size_t i = 0; i < all_insts.size(); ++i) {
    for (size_t j = i + 1; j < all_insts.size(); ++j) {
      const Instruction *i1 = all_insts[i];
      const Instruction *i2 = all_insts[j];

      // Skip if in same thread and ordered
      if (isInSameThread(i1, i2)) {
        continue;
      }

      if (!hasStructuralOrderRelation(i1, i2) &&
          !hasStructuralOrderRelation(i2, i1)) {
        // Store in canonical (pointer-sorted) order for O(1) symmetric lookup.
        const Instruction *ca = i1 < i2 ? i1 : i2;
        const Instruction *cb = i1 < i2 ? i2 : i1;
        m_mhp_pairs.insert({ca, cb});
        num_pairs++;
      }
    }
  }

  errs() << "Found " << num_pairs << " MHP pairs\n";
}

bool MHPAnalysis::mayHappenInParallel(const Instruction *i1,
                                      const Instruction *i2) const {
  // Symmetric memoization (keyed by pointer identity, order-independent).
  const Instruction *a = i1 < i2 ? i1 : i2;
  const Instruction *b = i1 < i2 ? i2 : i1;
  if (a && b) {
    auto it = m_mhp_cache.find({a, b});
    if (it != m_mhp_cache.end())
      return it->second;
  }

  // Basic checks: same instruction or same thread (accounting for
  // multi-instance threads)
  if (i1 == i2 || isInSameThread(i1, i2))
    return (a && b) ? (m_mhp_cache[{a, b}] = false) : false;

  // Initialization phase in main is single-threaded: instructions observed
  // before the first reachable thread/task creation in thread 0 cannot race
  // with child-thread nodes.
  ThreadID t1 = getThreadID(i1);
  ThreadID t2 = getThreadID(i2);
  if (m_openmp_task_exclusions.count(normalizeThreadPair(t1, t2))) {
    return (a && b) ? (m_mhp_cache[{a, b}] = false) : false;
  }
  const bool i1PreForkMain = isAlwaysPreForkMain(i1);
  const bool i2PreForkMain = isAlwaysPreForkMain(i2);
  if ((i1PreForkMain && t2 != 0) || (i2PreForkMain && t1 != 0))
    return (a && b) ? (m_mhp_cache[{a, b}] = false) : false;

  // Fast path: check precomputed MHP pairs
  if (isPrecomputedMHP(i1, i2))
    return (a && b) ? (m_mhp_cache[{a, b}] = true) : true;

  bool r = !hasStructuralOrderRelation(i1, i2) &&
           !hasStructuralOrderRelation(i2, i1);
  return (a && b) ? (m_mhp_cache[{a, b}] = r) : r;
}

bool MHPAnalysis::isPrecomputedMHP(const Instruction *i1,
                                   const Instruction *i2) const {
  // Pairs are stored in canonical (pointer-sorted) order so a single probe
  // suffices for a symmetric lookup.
  const Instruction *a = i1 < i2 ? i1 : i2;
  const Instruction *b = i1 < i2 ? i2 : i1;
  return m_mhp_pairs.count({a, b}) != 0;
}

InstructionSet
MHPAnalysis::getParallelInstructions(const Instruction *inst) const {
  InstructionSet result;

  for (const auto &pair : m_mhp_pairs) {
    if (pair.a == inst) {
      result.insert(pair.b);
    } else if (pair.b == inst) {
      result.insert(pair.a);
    }
  }

  return result;
}

bool MHPAnalysis::mustBeSequential(const Instruction *i1,
                                   const Instruction *i2) const {
  return !mayHappenInParallel(i1, i2);
}

ThreadID MHPAnalysis::getThreadID(const Instruction *inst) const {
  auto it = m_inst_to_thread.find(inst);
  return it != m_inst_to_thread.end() ? it->second : kUnknownThread;
}

InstructionSet MHPAnalysis::getInstructionsInThread(ThreadID tid) const {
  InstructionSet result;
  for (const auto &pair : m_inst_to_thread) {
    if (pair.second == tid) {
      result.insert(pair.first);
    }
  }
  return result;
}

std::set<LockID> MHPAnalysis::getLocksHeldAt(const Instruction *inst) const {
  // LockSet analysis is optional - only available if enabled
  if (m_lockset) {
    return m_lockset->getMayLockSetAt(inst);
  }
  // If lockset analysis not run, return empty set
  return std::set<LockID>();
}

ThreadID MHPAnalysis::allocateThreadID() { return m_next_thread_id++; }

void MHPAnalysis::mapInstructionToThread(const Instruction *inst,
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

bool MHPAnalysis::isInstructionThreadAmbiguous(const Instruction *inst) const {
  if (!inst) {
    return true;
  }
  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    return true;
  }
  return it->second == kUnknownThread;
}

bool MHPAnalysis::isMainThreadSpawnNode(const SyncNode *node) const {
  if (!node || !m_tfg || node->getThreadID() != 0) {
    return false;
  }

  for (SyncNode *succ : node->getSuccessors()) {
    if (succ->getThreadID() == 0) {
      continue;
    }
    if (m_tfg->getEdgeKind(node, succ) == EdgeKind::Create) {
      return true;
    }
  }

  return false;
}

void MHPAnalysis::recomputePreForkMainNodes() {
  m_pre_fork_main_nodes.clear();
  if (!m_tfg) {
    return;
  }

  auto canReachInMainThread = [](const SyncNode *from,
                                 const SyncNode *to) -> bool {
    if (!from || !to || from == to) {
      return false;
    }

    std::deque<const SyncNode *> worklist;
    std::unordered_set<const SyncNode *> visited;
    worklist.push_back(from);
    visited.insert(from);

    while (!worklist.empty()) {
      const SyncNode *current = worklist.front();
      worklist.pop_front();
      if (current == to) {
        return true;
      }
      for (SyncNode *succ : current->getSuccessors()) {
        if (succ->getThreadID() != 0) {
          continue;
        }
        if (visited.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }

    return false;
  };

  SyncNode *main_entry = m_tfg->getThreadEntryNode(0);
  if (!main_entry) {
    return;
  }

  std::vector<SyncNode *> thread_zero_nodes = m_tfg->getNodesInThread(0);
  std::vector<SyncNode *> spawn_nodes;
  spawn_nodes.reserve(thread_zero_nodes.size());
  for (SyncNode *node : thread_zero_nodes) {
    if (isMainThreadSpawnNode(node)) {
      spawn_nodes.push_back(node);
    }
  }

  for (SyncNode *node : thread_zero_nodes) {
    if (node != main_entry && !canReachInMainThread(main_entry, node)) {
      continue;
    }

    bool reachable_from_spawn = false;
    for (SyncNode *spawn_node : spawn_nodes) {
      if (spawn_node == node || canReachInMainThread(spawn_node, node)) {
        reachable_from_spawn = true;
        break;
      }
    }

    if (!reachable_from_spawn) {
      m_pre_fork_main_nodes.insert(node);
    }
  }
}

bool MHPAnalysis::isAlwaysPreForkMain(const Instruction *inst) const {
  if (!inst || !m_tfg) {
    return false;
  }

  std::vector<SyncNode *> nodes = m_tfg->getNodes(inst, 0);
  if (nodes.empty()) {
    return false;
  }
  for (SyncNode *node : nodes) {
    if (!m_pre_fork_main_nodes.count(node)) {
      return false;
    }
  }
  return true;
}

bool MHPAnalysis::hasStructuralOrderRelation(const Instruction *i1,
                                             const Instruction *i2) const {
  if (isInstructionThreadAmbiguous(i1) || isInstructionThreadAmbiguous(i2)) {
    return false;
  }

  auto it = m_order_cache.find({i1, i2});
  if (it != m_order_cache.end())
    return it->second;

  std::vector<SyncNode *> start_nodes = m_tfg->getNodes(i1, getThreadID(i1));
  std::vector<SyncNode *> end_nodes = m_tfg->getNodes(i2, getThreadID(i2));

  if (start_nodes.empty() || end_nodes.empty() || i1 == i2) {
    return (m_order_cache[{i1, i2}] = false);
  }

  auto can_reach = [this](SyncNode *start, SyncNode *end) -> bool {
    if (!start || !end) {
      return false;
    }
    if (m_tfg->hasReachabilityIndex()) {
      return m_tfg->canReach(start, end);
    }

    std::deque<SyncNode *> worklist;
    std::unordered_set<SyncNode *> visited;
    worklist.push_back(start);
    visited.insert(start);
    while (!worklist.empty()) {
      SyncNode *current = worklist.front();
      worklist.pop_front();
      if (current == end) {
        return true;
      }
      for (SyncNode *succ : current->getSuccessors()) {
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
  };

  bool tfg_orders_pair = true;
  for (SyncNode *start_node : start_nodes) {
    for (SyncNode *end_node : end_nodes) {
      if (!can_reach(start_node, end_node)) {
        tfg_orders_pair = false;
        break;
      }
    }
    if (!tfg_orders_pair) {
      break;
    }
  }

  if (tfg_orders_pair) {
    return (m_order_cache[{i1, i2}] = true);
  }
  return (m_order_cache[{i1, i2}] = false);
}

bool MHPAnalysis::isInSameThread(const Instruction *i1,
                                 const Instruction *i2) const {
  ThreadID t1 = getThreadID(i1);
  ThreadID t2 = getThreadID(i2);

  if (t1 == kUnknownThread || t2 == kUnknownThread) {
    return false;
  }

  if (t1 != t2) {
    return false;
  }

  // If they are in the same thread, we must check if this thread
  // can have multiple active instances (e.g., created in a loop).
  // If so, two instructions from the "same" static thread can run in parallel.
  if (m_multi_instance_threads.count(t1)) {
    return false; // Treat as potentially parallel
  }

  return true;
}

// ============================================================================
// Fork-Join Helper Methods
// ============================================================================

bool MHPAnalysis::isAncestorThread(ThreadID ancestor,
                                   ThreadID descendant) const {
  ThreadID current = descendant;
  while (m_thread_parents.find(current) != m_thread_parents.end()) {
    ThreadID parent = m_thread_parents.at(current);
    if (parent == ancestor) {
      return true;
    }
    current = parent;
  }
  return false;
}

bool MHPAnalysis::isForkSite(const Instruction *inst) const {
  return m_fork_to_thread.find(inst) != m_fork_to_thread.end();
}

bool MHPAnalysis::isJoinSite(const Instruction *inst) const {
  return m_join_to_thread.find(inst) != m_join_to_thread.end();
}

ThreadID MHPAnalysis::getForkedThreadID(const Instruction *fork_inst) const {
  auto it = m_fork_to_thread.find(fork_inst);
  return it != m_fork_to_thread.end() ? it->second : 0;
}

ThreadID MHPAnalysis::getJoinedThreadID(const Instruction *join_inst) const {
  auto it = m_join_to_thread.find(join_inst);
  return it != m_join_to_thread.end() ? it->second : 0;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

void MHPAnalysis::Statistics::print(raw_ostream &os) const {
  os << "MHP Analysis Statistics:\n";
  os << "========================\n";
  os << "Threads:          " << num_threads << "\n";
  os << "Forks:            " << num_forks << "\n";
  os << "Joins:            " << num_joins << "\n";
  os << "Locks:            " << num_locks << "\n";
  os << "Unlocks:          " << num_unlocks << "\n";
  os << "Regions:          " << num_regions << "\n";
  os << "MHP Pairs:        " << num_mhp_pairs << "\n";
  os << "Ordered Pairs:    " << num_ordered_pairs << "\n";
}

MHPAnalysis::Statistics MHPAnalysis::getStatistics() const {
  Statistics stats{};

  if (m_tfg) {
    stats.num_threads = m_tfg->getAllThreads().size();
    stats.num_forks = m_tfg->getNodesOfType(SyncNodeType::THREAD_FORK).size();
    stats.num_joins = m_tfg->getNodesOfType(SyncNodeType::THREAD_JOIN).size();
    stats.num_locks = m_tfg->getNodesOfType(SyncNodeType::LOCK_ACQUIRE).size();
    stats.num_unlocks =
        m_tfg->getNodesOfType(SyncNodeType::LOCK_RELEASE).size();
  }

  if (m_region_analysis) {
    stats.num_regions = m_region_analysis->getAllRegions().size();
  }

  stats.num_mhp_pairs = m_mhp_pairs.size();

  return stats;
}

void MHPAnalysis::printStatistics(raw_ostream &os) const {
  auto stats = getStatistics();
  stats.print(os);
}

void MHPAnalysis::printResults(raw_ostream &os) const {
  os << "\n=== MHP Analysis Results ===\n\n";

  printStatistics(os);

  os << "\n=== Thread Flow Graph ===\n";
  if (m_tfg) {
    m_tfg->print(os);
  }

  os << "\n=== Thread Region Analysis ===\n";
  if (m_region_analysis) {
    m_region_analysis->print(os);
  }

  // Optional: Lock Set Analysis (only if enabled)
  if (m_lockset) {
    os << "\n=== Lock Set Analysis ===\n";
    m_lockset->print(os);
  }

  os << "\n=== MHP Pairs (sample) ===\n";
  size_t count = 0;
  for (const auto &pair : m_mhp_pairs) {
    os << "MHP: ";
    pair.a->print(os);
    os << " ||| ";
    pair.b->print(os);
    os << "\n";

    if (++count >= 20) {
      os << "... (" << (m_mhp_pairs.size() - 20) << " more pairs)\n";
      break;
    }
  }
}

void MHPAnalysis::dumpThreadFlowGraph(const std::string &filename) const {
  if (m_tfg) {
    m_tfg->dumpToFile(filename);
    errs() << "Thread flow graph dumped to " << filename << "\n";
  }
}

void MHPAnalysis::dumpMHPMatrix(raw_ostream &os) const {
  os << "MHP Matrix:\n";
  os << "===========\n";
  // Matrix visualization would go here
  // This is a placeholder for a more sophisticated visualization
}

// =========================================================================
// Dominator helpers
// =========================================================================

const DominatorTree &MHPAnalysis::getDomTree(const Function *func) const {
  auto it = m_dom_cache.find(func);
  if (it != m_dom_cache.end()) {
    return *(it->second);
  }
  auto DT = std::make_unique<DominatorTree>();
  DT->recalculate(*const_cast<Function *>(func));
  auto *dtPtr = DT.get();
  m_dom_cache[func] = std::move(DT);
  return *dtPtr;
}

// =========================================================================
// Program Order Helpers (Precise Happens-Before for Same Thread)
// =========================================================================

std::vector<SyncNode *>
MHPAnalysis::getBarrierContinuations(const Instruction *barrier_inst) const {
  std::vector<SyncNode *> continuations;
  if (!barrier_inst) {
    return continuations;
  }

  if (const Instruction *next = barrier_inst->getNextNode()) {
    for (SyncNode *next_node :
         m_tfg->getNodes(next, getThreadID(barrier_inst))) {
      continuations.push_back(next_node);
    }
  }

  if (barrier_inst->isTerminator()) {
    for (const BasicBlock *succ : successors(barrier_inst->getParent())) {
      if (succ->empty()) {
        continue;
      }
      for (SyncNode *succ_node :
           m_tfg->getNodes(&succ->front(), getThreadID(barrier_inst))) {
        continuations.push_back(succ_node);
      }
    }
  }

  if (continuations.empty()) {
    for (SyncNode *self :
         m_tfg->getNodes(barrier_inst, getThreadID(barrier_inst))) {
      continuations.push_back(self);
    }
  }

  return continuations;
}
