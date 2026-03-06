/**
 * @file MHPAnalysis.cpp
 * @brief Implementation of May-Happen-in-Parallel Analysis
 *
 * This analysis constructs a Thread Flow Graph (TFG) to determine which
 * instructions may execute concurrently.
 *
 * Soundness Properties:
 * - Default Safety: The analysis is conservative (safe) for race detection.
 *   It assumes two instructions MHP unless a Happens-Before (HB) relation is
 * proven.
 * - Synchronization:
 *   - Fork/Join: Precisely models ancestor relationships.
 *   - Locks: Uses LockSet analysis (if enabled) to rule out parallelism guarded
 * by common locks.
 *   - Condition Variables: Conservatively assumes a signal may wake any wait.
 *   - Barriers: Enforces program order across barriers.
 * - Thread Instances: Conservatively assumes spawned threads may have multiple
 * instances.
 *
 * Author: rainoftime
 */

#include "Analysis/Concurrency/MHP/MHPAnalysis.h"

#include "Analysis/Concurrency/Utils/ThreadLocalAnalysis.h"

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
  SyncNode *node = m_tfg.getNode(inst);
  if (!node)
    return false;

  SyncNodeType type = node->getType();
  return isSynchronizationNode(type) || isThreadBoundaryNode(type);
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

      region->end_node = m_tfg.getNode(&inst);
      flush_region(region);
      pending_start = m_tfg.getNode(&inst);
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
      if (j < kMaxRegions) {
        region_i->must_precede_bits.set(j);
      }
    }
    for (size_t j : region_i->must_follow) {
      if (j < kMaxRegions) {
        region_i->must_follow_bits.set(j);
      }
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
      if (j < kMaxRegions) {
        region_i->may_be_parallel_bits.set(j);
      }
      if (i < kMaxRegions) {
        region_j->may_be_parallel_bits.set(i);
      }
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
  m_tls_analysis = std::make_unique<ThreadLocal::ThreadLocalAnalysis>(m_module);
  m_call_graph = std::make_unique<CallGraph>(m_module);
}

MHPAnalysis::~MHPAnalysis() = default;

void MHPAnalysis::analyze() {
  errs() << "Starting MHP Analysis...\n";

  // Run TLS analysis first
  m_tls_analysis->analyze();

  buildThreadFlowGraph();

  // Optional: LockSet analysis for more precise reasoning
  if (m_enable_lockset_analysis) {
    analyzeLockSets();
  }

  computeAtomicHappensBefore();
  analyzeThreadRegions();
  if (m_precompute_mhp_pairs) {
    // Optionally compute transitive closure for faster HB queries
    // (only beneficial if we'll do many MHP queries)
    computeHappensBeforeTransitiveClosure();
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

  // Main thread (thread 0)
  m_tfg->addThread(0, main_func);
  processFunction(main_func, 0, true);

  // Build reachability index for faster HB queries
  m_tfg->buildReachabilityIndex();

  errs() << "Thread Flow Graph built with " << m_tfg->getAllNodes().size()
         << " nodes\n";
}

void MHPAnalysis::processFunction(const Function *func, ThreadID tid,
                                  bool inPreForkMainPhase) {
  if (!func || func->isDeclaration())
    return;

  // Avoid re-processing functions for the same thread context
  if (m_visited_functions_by_thread[tid].count(func)) {
    return;
  }
  m_visited_functions_by_thread[tid].insert(func);

  // --- Pass 1: Create all nodes for this function ---
  bool preForkMainPhasePass1 = inPreForkMainPhase && (tid == 0);
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      mapInstructionToThread(&inst, tid);
      if (preForkMainPhasePass1) {
        m_pre_fork_main_insts.insert(&inst);
      }
      SyncNodeType node_type = SyncNodeType::REGULAR_INST;

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        (void)cb;
        if (m_thread_api->isTDFork(&inst)) {
          node_type = SyncNodeType::THREAD_FORK;
          // After the first fork in thread 0, subsequent instructions are no
          // longer in initialization phase.
          if (tid == 0) {
            preForkMainPhasePass1 = false;
          }
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
        } else if (m_thread_api->isTDBarWait(&inst)) {
          node_type = SyncNodeType::BARRIER_WAIT;
        }
      }
      m_tfg->createNode(&inst, node_type, tid);
    }
  }

  // --- Pass 2: Add edges and handle synchronization logic ---
  // Set entry node to first instruction in entry block
  SyncNode *entry_node = nullptr;
  if (!func->empty() && !func->front().empty()) {
    entry_node = m_tfg->getNode(&func->front().front());
    if (entry_node) {
      m_tfg->setThreadEntryNode(tid, entry_node);
    }
  }

  SyncNode *exit_node = nullptr;

  bool preForkMainPhasePass2 = inPreForkMainPhase && (tid == 0);
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      SyncNode *node = m_tfg->getNode(&inst);
      if (!node)
        continue;

      // Update exit node to last instruction we see
      exit_node = node;

      // Add intra-block edges
      if (&inst != &bb.front()) {
        const Instruction *prev_inst = inst.getPrevNode();
        if (prev_inst) {
          SyncNode *prev_node = m_tfg->getNode(prev_inst);
          if (prev_node)
            m_tfg->addIntraThreadEdge(prev_node, node);
        }
      }

      // Add inter-block (CFG) edges
      if (inst.isTerminator()) {
        for (const BasicBlock *succ : successors(inst.getParent())) {
          if (!succ->empty()) {
            SyncNode *succ_node = m_tfg->getNode(&succ->front());
            if (succ_node)
              m_tfg->addIntraThreadEdge(node, succ_node);
          }
        }
      }

      // Handle synchronization logic for special instructions
      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        if (m_thread_api->isTDFork(&inst)) {
          handleThreadFork(&inst, node, tid);
          if (tid == 0) {
            preForkMainPhasePass2 = false;
          }
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
        } else if (m_thread_api->isTDBarWait(&inst)) {
          handleBarrier(&inst, node);
        } else {
          // Handle both direct and indirect calls
          const Function *callee = cb->getCalledFunction();

          if (!callee) {
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
                        // Process this possible callee
                        processFunction(possibleCallee, tid,
                                        preForkMainPhasePass2);
                      }
                    }
                  }
                }
              }
            }
          } else if (!callee->isDeclaration()) {
            // Direct call to a defined function
            processFunction(callee, tid, preForkMainPhasePass2);
          }
        }
      }
    }
  }

  // Set exit node if we haven't set it yet
  if (exit_node && !m_tfg->getThreadExitNode(tid)) {
    m_tfg->setThreadExitNode(tid, exit_node);
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
    m_pthread_value_to_thread[pthread_ptr] = new_tid;
    m_thread_to_pthread_value[new_tid] = pthread_ptr;

    // Also track the store if it exists (for later load tracking)
    // In a more sophisticated implementation, we'd do def-use chain analysis
  }

  // Get the forked function
  const Value *forked_fun_val = m_thread_api->getForkedFun(fork_inst);
  if (const Function *forked_fun = dyn_cast_or_null<Function>(forked_fun_val)) {
    m_tfg->addThread(new_tid, forked_fun);

    // Process the forked function
    processFunction(forked_fun, new_tid);

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
  // Use a worklist to trace back through the def-use chain of the value.
  std::deque<const Value *> worklist;
  worklist.push_back(val);
  std::set<const Value *> visited;

  while (!worklist.empty()) {
    const Value *v = worklist.front();
    worklist.pop_front();

    if (visited.count(v)) {
      continue;
    }
    visited.insert(v);

    // Base case 1: We found the allocation site of the pthread_t variable.
    if (isa<AllocaInst>(v)) {
      return v;
    }

    // Base case 2: We found a value that is already directly mapped to a thread
    // ID.
    if (m_pthread_value_to_thread.count(v)) {
      return v;
    }

    // Recursive step: add operands to the worklist.
    if (const LoadInst *load = dyn_cast<LoadInst>(v)) {
      worklist.push_back(load->getPointerOperand());
    } else if (const StoreInst *store = dyn_cast<StoreInst>(v)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
    } else if (const BitCastInst *cast = dyn_cast<BitCastInst>(v)) {
      worklist.push_back(cast->getOperand(0));
    } else if (const GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(v)) {
      worklist.push_back(gep->getPointerOperand());
    } else if (const PHINode *phi = dyn_cast<PHINode>(v)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
    } else if (const SelectInst *select = dyn_cast<SelectInst>(v)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
    } else if (const Argument *arg = dyn_cast<Argument>(v)) {
      const Function *parent = arg->getParent();
      if (!parent) {
        continue;
      }
      if (parent->hasAddressTaken()) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (!cb || arg->getArgNo() >= cb->arg_size()) {
            continue;
          }
          worklist.push_back(cb->getArgOperand(arg->getArgNo()));
        }
      }
    } else if (const Instruction *inst = dyn_cast<Instruction>(v)) {
      // General case for other instructions, trace all operands.
      for (const Use &use : inst->operands()) {
        worklist.push_back(use.get());
      }
    }
  }

  return nullptr; // Could not trace back to a known origin.
}

void MHPAnalysis::handleThreadJoin(const Instruction *join_inst, SyncNode *node,
                                   ThreadID parent_tid) {
  // Track which thread is being joined using value analysis
  // pthread_join takes the pthread_t value (not pointer) as first argument

  const Value *joined_thread_val = m_thread_api->getJoinedThread(join_inst);
  ThreadID joined_tid = 0;
  bool found_thread = false;

  if (joined_thread_val) {
    // Use the improved tracing function to find the origin of the pthread_t
    // value.
    const Value *pthread_t_origin = tracePthreadT(joined_thread_val);

    if (pthread_t_origin) {
      auto it = m_pthread_value_to_thread.find(pthread_t_origin);
      if (it != m_pthread_value_to_thread.end()) {
        joined_tid = it->second;
        found_thread = true;
        // Cache the result for the original value to speed up future lookups.
        if (pthread_t_origin != joined_thread_val) {
          m_pthread_value_to_thread[joined_thread_val] = joined_tid;
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
    // Fallback: couldn't determine specific thread, so conservatively
    // assume it could be any child thread of the current thread
    (void)parent_tid;
    // Unknown join target: skip adding HB edges to avoid unsound ordering.
  }
}

void MHPAnalysis::handleLockAcquire(const Instruction *lock_inst,
                                    SyncNode *node) {
  const Value *lock = m_thread_api->getLockVal(lock_inst);
  node->setLockValue(lock);
}

void MHPAnalysis::handleLockRelease(const Instruction *unlock_inst,
                                    SyncNode *node) {
  const Value *lock = m_thread_api->getLockVal(unlock_inst);
  node->setLockValue(lock);
}

void MHPAnalysis::handleCondWait(const Instruction *wait_inst, SyncNode *node) {
  // Condition variable wait handling with improved precision
  // pthread_cond_wait atomically releases the mutex and waits for a signal
  // When woken up, it reacquires the mutex
  //
  // Improvement: Track mutex association to enable more precise signal-wait
  // pairing. A signal can only wake a wait if they use the same mutex (per
  // POSIX semantics).

  const Value *cond = m_thread_api->getCondVal(wait_inst);
  const Value *mutex = m_thread_api->getCondMutex(wait_inst);

  node->setCondValue(cond);
  node->setLockValue(mutex);

  // Track this wait with its associated mutex for precise pairing
  m_condvar_waits[cond].push_back(wait_inst);

  // Add happens-before edges from signals on the same condition variable
  // Improved: Only add edges from signals that could actually wake this wait
  auto it = m_condvar_signals.find(cond);
  if (it != m_condvar_signals.end()) {
    for (const Instruction *signal_inst : it->second) {
      if (isInSameThread(signal_inst, wait_inst)) {
        continue;
      }

      SyncNode *signal_node = m_tfg->getNode(signal_inst);
      if (!signal_node) {
        continue;
      }

      // Check mutex compatibility: signal and wait should use same mutex
      // for the signal to potentially wake the wait (POSIX requirement)
      const Value *signal_mutex = signal_node->getLockValue();
      bool mutex_compatible = true;

      if (mutex && signal_mutex && m_alias_analysis) {
        // If mutexes don't alias, this signal cannot wake this wait
        if (!m_alias_analysis->mayAlias(mutex, signal_mutex)) {
          mutex_compatible = false;
        }
      }

      if (mutex_compatible) {
        m_tfg->addInterThreadEdge(signal_node, node);
      }
    }
  }
}

void MHPAnalysis::handleCondSignal(const Instruction *signal_inst,
                                   SyncNode *node) {
  // Condition variable signal/broadcast handling with improved precision
  // Wakes up one or more waiting threads
  //
  // Improvement: Only pair signals with waits that use compatible mutexes,
  // and track broadcast vs signal for different precision levels.

  // B2 fix: use getCondVal (not getLockVal) for the condition variable.
  // getLockVal asserts isTDAcquire||isTDRelease and would crash on a signal.
  const Value *cond = m_thread_api->getCondVal(signal_inst);
  // The mutex is not directly available from a signal instruction; leave null.
  const Value *mutex = nullptr;

  node->setCondValue(cond);
  if (mutex) {
    node->setLockValue(mutex);
  }

  // Track this signal for happens-before analysis
  m_condvar_signals[cond].push_back(signal_inst);

  // Check if this is a broadcast (wakes all waiters) or signal (wakes one)
  // pthread_cond_signal vs pthread_cond_broadcast
  bool is_broadcast = false;
  if (const CallBase *cb = dyn_cast<CallBase>(signal_inst)) {
    if (const Function *callee = cb->getCalledFunction()) {
      StringRef name = callee->getName();
      is_broadcast = name.contains("broadcast");
    }
  }

  // Add happens-before edges to waits on the same condition variable
  auto it = m_condvar_waits.find(cond);
  if (it != m_condvar_waits.end()) {
    for (const Instruction *wait_inst : it->second) {
      if (isInSameThread(signal_inst, wait_inst)) {
        continue;
      }

      SyncNode *wait_node = m_tfg->getNode(wait_inst);
      if (!wait_node) {
        continue;
      }

      // Check mutex compatibility
      const Value *wait_mutex = wait_node->getLockValue();
      bool mutex_compatible = true;

      if (mutex && wait_mutex && m_alias_analysis) {
        if (!m_alias_analysis->mayAlias(mutex, wait_mutex)) {
          mutex_compatible = false;
        }
      }

      if (mutex_compatible) {
        // For broadcast: add edge to all compatible waits
        // For signal: conservatively add edge to all compatible waits
        // (a more precise analysis would track waiter queues)
        (void)is_broadcast; // Reserved for future optimization
        m_tfg->addInterThreadEdge(node, wait_node);
      }
    }
  }
}

void MHPAnalysis::handleBarrier(const Instruction *barrier_inst,
                                SyncNode *node) {
  const Value *barrier = m_thread_api->getBarrierVal(barrier_inst);
  if (!barrier) {
    barrier = barrier_inst;
  }
  node->setLockValue(barrier); // Reuse lock field for barrier value

  BarrierParticipant current;
  current.arrival = barrier_inst;
  current.continuations = getBarrierContinuations(barrier_inst);

  auto &participants = m_barrier_waits[barrier];
  for (const BarrierParticipant &previous : participants) {
    if (!previous.arrival || isInSameThread(previous.arrival, barrier_inst)) {
      continue;
    }

    SyncNode *prev_node = m_tfg->getNode(previous.arrival);
    if (!prev_node) {
      continue;
    }

    for (SyncNode *cont : current.continuations) {
      if (cont) {
        m_tfg->addInterThreadEdge(prev_node, cont);
      }
    }

    for (SyncNode *cont : previous.continuations) {
      if (cont) {
        m_tfg->addInterThreadEdge(node, cont);
      }
    }
  }

  participants.push_back(std::move(current));
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

  if (num_regions > kMaxRegions) {
    errs() << "Warning: Too many regions (" << num_regions << "), falling back "
           << "to instruction-level computation\n";
    computeMHPPairsInstructionLevel();
    return;
  }

  // Phase 1: Build per-thread region bitvectors for fast filtering
  std::unordered_map<ThreadID, RegionBitVector> thread_regions;
  for (size_t i = 0; i < num_regions; ++i) {
    thread_regions[regions[i]->thread_id].set(i);
  }

  // Phase 2: Compute MHP region pairs using bitvector operations
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

      // Use bitvector test for O(1) check
      if (region_i->may_be_parallel_bits.test(j)) {
        mhp_region_pairs.push_back({i, j});
      }
    }
  }

  errs() << "Found " << mhp_region_pairs.size() << " MHP region pairs\n";

  // Phase 3: Expand region-level MHP to instruction-level MHP
  // Uses the original isOrderedByLocks for correct handling of read/write locks
  size_t num_pairs = 0;
  size_t lock_filtered = 0;
  size_t hb_filtered = 0;

  for (const auto &pair : mhp_region_pairs) {
    size_t ri = pair.first, rj = pair.second;
    const auto &region_i = regions[ri];
    const auto &region_j = regions[rj];

    for (const Instruction *inst_i : region_i->instructions) {
      for (const Instruction *inst_j : region_j->instructions) {
        // Check if lock-protected using the original method
        // which correctly handles read/write lock semantics
        if (isOrderedByLocks(inst_i, inst_j)) {
          lock_filtered++;
          continue;
        }

        // Check happens-before
        if (hasHappenBeforeRelation(inst_i, inst_j) ||
            hasHappenBeforeRelation(inst_j, inst_i)) {
          hb_filtered++;
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
         << "(filtered " << lock_filtered << " by locks, " << hb_filtered
         << " by HB)\n";
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

      // Check if they may happen in parallel
      if (!hasHappenBeforeRelation(i1, i2) &&
          !hasHappenBeforeRelation(i2, i1)) {
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
  // before the first pthread_create in thread 0 cannot race with child threads.
  ThreadID t1 = getThreadID(i1);
  ThreadID t2 = getThreadID(i2);
  const bool i1PreForkMain = m_pre_fork_main_insts.count(i1) != 0;
  const bool i2PreForkMain = m_pre_fork_main_insts.count(i2) != 0;
  if ((i1PreForkMain && t2 != 0) || (i2PreForkMain && t1 != 0))
    return (a && b) ? (m_mhp_cache[{a, b}] = false) : false;

  // Fast path: check precomputed MHP pairs
  if (isPrecomputedMHP(i1, i2))
    return (a && b) ? (m_mhp_cache[{a, b}] = true) : true;

  // Special case: if both instructions are from the same multi-instance thread,
  // they can run in parallel (different instances) unless explicitly ordered
  // by inter-thread synchronization
  if (t1 == t2 && t1 != 0 && m_multi_instance_threads.count(t1)) {
    // For multi-instance threads, intra-thread program order does not prevent
    // parallelism between different dynamic thread instances.
    // However, mutual exclusion via locks still applies.
    bool r = !isOrderedByLocks(i1, i2);
    return (a && b) ? (m_mhp_cache[{a, b}] = r) : r;
  }

  // Soundness: This is the core conservative check.
  // If we cannot PROVE a happens-before relation, and we cannot PROVE they are
  // mutually exclusive (via locks), we MUST assume they can run in parallel.
  // This ensures we don't miss any potential races (over-approximation).
  bool r = !hasHappenBeforeRelation(i1, i2) &&
           !hasHappenBeforeRelation(i2, i1) && !isOrderedByLocks(i1, i2);
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

bool MHPAnalysis::mustPrecede(const Instruction *i1,
                              const Instruction *i2) const {
  return hasHappenBeforeRelation(i1, i2);
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

bool MHPAnalysis::isMustIntraThreadEdge(const SyncNode *from,
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

bool MHPAnalysis::hasHappenBeforeRelation(const Instruction *i1,
                                          const Instruction *i2) const {
  if (isInstructionThreadAmbiguous(i1) || isInstructionThreadAmbiguous(i2)) {
    return false;
  }

  auto it = m_hb_cache.find({i1, i2});
  if (it != m_hb_cache.end())
    return it->second;

  // Fast path: use transitive closure if available
  if (m_hb_closure_computed) {
    auto closure_it = m_hb_transitive_closure.find(i1);
    if (closure_it != m_hb_transitive_closure.end()) {
      bool result = closure_it->second.count(i2) > 0;
      return (m_hb_cache[{i1, i2}] = result);
    }
  }

  SyncNode *startNode = m_tfg->getNode(i1);
  SyncNode *endNode = m_tfg->getNode(i2);

  if (!startNode || !endNode || i1 == i2) {
    return (m_hb_cache[{i1, i2}] = false);
  }

  // Use indexed reachability when available (O(1) for intra-thread)
  if (m_tfg->hasReachabilityIndex()) {
    bool result = m_tfg->canReach(startNode, endNode);
    return (m_hb_cache[{i1, i2}] = result);
  }

  // Fallback: BFS on TFG (slower but always works)
  std::deque<SyncNode *> worklist;
  worklist.push_back(startNode);
  std::unordered_set<SyncNode *> visited;
  visited.insert(startNode);

  while (!worklist.empty()) {
    SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == endNode) {
      return (m_hb_cache[{i1, i2}] = true);
    }

    for (SyncNode *succ : current->getSuccessors()) {
      const Instruction *succ_inst = succ->getInstruction();
      if (succ_inst && isInstructionThreadAmbiguous(succ_inst)) {
        continue;
      }

      if (visited.insert(succ).second)
        worklist.push_back(succ);
    }
  }

  return (m_hb_cache[{i1, i2}] = false);
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

bool MHPAnalysis::isOrderedByLocks(const Instruction *i1,
                                   const Instruction *i2) const {
  // Use LockSetAnalysis if available to determine if both instructions
  // are protected by the same exclusive lock
  if (!m_lockset) {
    return false; // Conservative: assume no lock-based ordering
  }

  // Get lock sets at both instructions
  LockSet locks1 = m_lockset->getMayLockSetAt(i1);
  LockSet locks2 = m_lockset->getMayLockSetAt(i2);

  // Check for common exclusive locks
  // Two accesses under the same exclusive lock cannot be MHP
  for (LockID lock1 : locks1) {
    for (LockID lock2 : locks2) {
      // Check if locks may alias
      if (lock1 == lock2) {
        // Same lock: check if it's exclusive (not read-locked)
        LockSet read_locks1 = m_lockset->getMayReadLockSetAt(i1);
        LockSet read_locks2 = m_lockset->getMayReadLockSetAt(i2);

        // Both under read lock: can be parallel (readers-writers semantics)
        if (read_locks1.count(lock1) && read_locks2.count(lock2)) {
          continue;
        }

        // At least one is write-locked: mutually exclusive
        return true;
      }

      // Use alias analysis to check if locks may be the same
      if (m_alias_analysis && m_alias_analysis->mayAlias(lock1, lock2)) {
        // Potentially same lock, check read/write status
        LockSet read_locks1 = m_lockset->getMayReadLockSetAt(i1);
        LockSet read_locks2 = m_lockset->getMayReadLockSetAt(i2);

        bool is_read1 = read_locks1.count(lock1) > 0;
        bool is_read2 = read_locks2.count(lock2) > 0;

        // Both read-locked: can be parallel
        if (is_read1 && is_read2) {
          continue;
        }

        // At least one write-locked: conservatively assume ordered
        return true;
      }
    }
  }

  return false; // No common lock found
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

const PostDominatorTree &
MHPAnalysis::getPostDomTree(const Function *func) const {
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

bool MHPAnalysis::dominates(const Instruction *a, const Instruction *b) const {
  if (!a || !b)
    return false;
  const Function *fa = a->getFunction();
  const Function *fb = b->getFunction();
  if (fa != fb)
    return false;
  const DominatorTree &DT = getDomTree(fa);
  return DT.dominates(a, b);
}

// =========================================================================
// Program Order Helpers (Precise Happens-Before for Same Thread)
// =========================================================================

bool MHPAnalysis::isBackEdge(const BasicBlock *from, const BasicBlock *to,
                             const DominatorTree &DT) const {
  // A back edge is an edge from 'from' to 'to' where 'to' dominates 'from'
  // This captures loop back edges
  return DT.dominates(to, from);
}

bool MHPAnalysis::isReachableWithoutBackEdges(const Instruction *from,
                                              const Instruction *to) const {
  // Checks reachability in the CFG ignoring back-edges (loops).
  // This essentially checks "program text order" (lexical/topological order).
  //
  // Why ignore back-edges?
  // - We want to know if 'from' *must* precede 'to' in a single linear
  // execution trace.
  // - With loops, 'to' might execute before 'from' in a subsequent iteration
  // (cross-iteration),
  //   but for defining a "happens-before" relation that rules out parallelism
  //   within the same conceptual thread instance, we focus on the acyclic
  //   backbone.
  // - This is a conservative approximation for "program order" to avoid cycles
  // in HB graph.

  if (!from || !to)
    return false;

  if (from == to)
    return false;

  const Function *func = from->getFunction();
  if (func != to->getFunction())
    return false;

  const BasicBlock *fromBB = from->getParent();
  const BasicBlock *toBB = to->getParent();

  // Quick check: if in same basic block, check instruction order
  if (fromBB == toBB) {
    // Check if 'from' appears before 'to' in the basic block
    for (const Instruction &inst : *fromBB) {
      if (&inst == from)
        return true; // from comes first
      if (&inst == to)
        return false; // to comes first
    }
    return false;
  }

  // Different basic blocks: perform BFS without following back edges
  const DominatorTree &DT = getDomTree(func);

  std::unordered_set<const BasicBlock *> visited;
  std::vector<const BasicBlock *> worklist;

  // Start from the basic block containing 'from'
  // But only consider successors after 'from' in that block
  worklist.push_back(fromBB);
  visited.insert(fromBB);

  while (!worklist.empty()) {
    const BasicBlock *current = worklist.back();
    worklist.pop_back();

    // Check successors
    for (const BasicBlock *succ : successors(current)) {
      // Skip back edges (loop back edges)
      if (isBackEdge(current, succ, DT)) {
        continue;
      }

      // If we reached the target block, check if we can reach 'to'
      if (succ == toBB) {
        return true;
      }

      // Continue exploring if not visited
      if (visited.find(succ) == visited.end()) {
        visited.insert(succ);
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

void MHPAnalysis::computeAtomicHappensBefore() {
  errs() << "Computing Atomic Happens-Before...\n";

  // Phase 1: Collect all atomic instructions if not already done
  if (m_atomic_instructions.empty()) {
    for (Function &F : m_module) {
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        if (CppAtomics::isAtomic(&*I)) {
          m_atomic_instructions.push_back(&*I);
        }
      }
    }
  }

  // Clear the old pairs and rebuild
  m_atomic_hb_pairs.clear();
  m_atomic_sync_witnesses.clear();
  size_t pairs_found = 0;

  // Phase 2: Find release-acquire pairs for synchronizing variables.
  // This used to be O(#atomic_insts^2). We reduce AA queries by bucketing
  // by atomic pointer value and querying aliasing per-pointer-bucket pair.
  using InstVec = std::vector<const Instruction *>;
  std::unordered_map<const Value *, InstVec> releasesByPtr;
  std::unordered_map<const Value *, InstVec> acquiresByPtr;
  releasesByPtr.reserve(m_atomic_instructions.size());
  acquiresByPtr.reserve(m_atomic_instructions.size());

  auto canonPtr = [](const Value *p) -> const Value * {
    return p ? p->stripPointerCasts() : nullptr;
  };

  for (const Instruction *inst : m_atomic_instructions) {
    const Value *ptr = canonPtr(CppAtomics::getAtomicPointer(inst));
    if (!ptr)
      continue;

    const auto mo = CppAtomics::getMemoryOrder(inst);
    const bool has_release =
        (mo == CppAtomics::MemoryOrder::Release ||
         mo == CppAtomics::MemoryOrder::AcquireRelease ||
         mo == CppAtomics::MemoryOrder::SequentiallyConsistent);
    const bool has_acquire =
        (mo == CppAtomics::MemoryOrder::Acquire ||
         mo == CppAtomics::MemoryOrder::AcquireRelease ||
         mo == CppAtomics::MemoryOrder::SequentiallyConsistent);

    if (CppAtomics::isStore(inst) && has_release)
      releasesByPtr[ptr].push_back(inst);
    if (CppAtomics::isLoad(inst) && has_acquire)
      acquiresByPtr[ptr].push_back(inst);
  }

  std::vector<const Value *> releasePtrs;
  std::vector<const Value *> acquirePtrs;
  releasePtrs.reserve(releasesByPtr.size());
  acquirePtrs.reserve(acquiresByPtr.size());
  for (auto &kv : releasesByPtr)
    releasePtrs.push_back(kv.first);
  for (auto &kv : acquiresByPtr)
    acquirePtrs.push_back(kv.first);

  auto addEdge = [&](const Instruction *release_inst,
                     const Instruction *acquire_inst) {
    if (isInSameThread(release_inst, acquire_inst))
      return;
    m_atomic_hb_pairs.insert({release_inst, acquire_inst});
    m_atomic_sync_witnesses.push_back(
        {release_inst, acquire_inst,
         CppAtomics::getAtomicPointer(release_inst)
             ? CppAtomics::getAtomicPointer(release_inst)->stripPointerCasts()
             : nullptr});
    SyncNode *release_node = m_tfg->getNode(release_inst);
    SyncNode *acquire_node = m_tfg->getNode(acquire_inst);
    if (release_node && acquire_node)
      m_tfg->addInterThreadEdge(release_node, acquire_node);
    ++pairs_found;
  };

  // First, handle identical-pointer buckets without AA queries.
  for (const Value *p : releasePtrs) {
    auto itA = acquiresByPtr.find(p);
    if (itA == acquiresByPtr.end())
      continue;
    const InstVec &rels = releasesByPtr[p];
    const InstVec &acqs = itA->second;
    for (const Instruction *r : rels)
      for (const Instruction *a : acqs)
        addEdge(r, a);
  }

  // Then, handle cross-pointer aliasing (AA query per pointer-pair).
  for (const Value *rp : releasePtrs) {
    for (const Value *ap : acquirePtrs) {
      if (rp == ap)
        continue; // already handled
      if (m_alias_analysis && !m_alias_analysis->mayAlias(rp, ap))
        continue;
      const InstVec &rels = releasesByPtr[rp];
      const InstVec &acqs = acquiresByPtr[ap];
      for (const Instruction *r : rels)
        for (const Instruction *a : acqs)
          addEdge(r, a);
    }
  }

  // Phase 3: Fence-based synchronization
  computeFenceBasedHappensBefore();

  // Phase 4: Sequential consistency total order
  computeSeqCstTotalOrder();

  errs() << "Found " << pairs_found
         << " atomic happens-before pairs (release-acquire).\n";
}

std::vector<const Instruction *>
MHPAnalysis::collectFenceWitnesses(const Instruction *fence,
                                   bool require_release_semantics) const {
  std::vector<const Instruction *> witnesses;
  if (!fence) {
    return witnesses;
  }

  ThreadID fence_tid = getThreadID(fence);
  for (const Instruction *inst : m_atomic_instructions) {
    if (inst == fence || CppAtomics::isFence(inst)) {
      continue;
    }
    if (getThreadID(inst) != fence_tid) {
      continue;
    }

    if (require_release_semantics) {
      if (!CppAtomics::hasReleaseSemantics(inst) || !CppAtomics::isStore(inst)) {
        continue;
      }
      if (!hasHappenBeforeRelation(inst, fence)) {
        continue;
      }
    } else {
      if (!CppAtomics::hasAcquireSemantics(inst) || !CppAtomics::isLoad(inst)) {
        continue;
      }
      if (!hasHappenBeforeRelation(fence, inst)) {
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

std::vector<SyncNode *>
MHPAnalysis::getBarrierContinuations(const Instruction *barrier_inst) const {
  std::vector<SyncNode *> continuations;
  if (!barrier_inst) {
    return continuations;
  }

  if (const Instruction *next = barrier_inst->getNextNode()) {
    if (SyncNode *next_node = m_tfg->getNode(next)) {
      continuations.push_back(next_node);
    }
  }

  if (barrier_inst->isTerminator()) {
    for (const BasicBlock *succ : successors(barrier_inst->getParent())) {
      if (succ->empty()) {
        continue;
      }
      if (SyncNode *succ_node = m_tfg->getNode(&succ->front())) {
        continuations.push_back(succ_node);
      }
    }
  }

  if (continuations.empty()) {
    if (SyncNode *self = m_tfg->getNode(barrier_inst)) {
      continuations.push_back(self);
    }
  }

  return continuations;
}

void MHPAnalysis::computeFenceBasedHappensBefore() {
  std::vector<const Instruction *> release_fences;
  std::vector<const Instruction *> acquire_fences;

  for (const Instruction *inst : m_atomic_instructions) {
    if (CppAtomics::isFence(inst)) {
      if (CppAtomics::isFenceRelease(inst) || CppAtomics::isFenceAcqRel(inst) ||
          CppAtomics::isFenceSeqCst(inst)) {
        release_fences.push_back(inst);
      }

      if (CppAtomics::isFenceAcquire(inst) || CppAtomics::isFenceAcqRel(inst) ||
          CppAtomics::isFenceSeqCst(inst)) {
        acquire_fences.push_back(inst);
      }
    }
  }

  size_t fence_pairs = 0;

  for (const Instruction *rel_fence : release_fences) {
    const auto release_ops = collectFenceWitnesses(rel_fence, true);
    if (release_ops.empty()) {
      continue;
    }

    for (const Instruction *acq_fence : acquire_fences) {
      if (isInSameThread(rel_fence, acq_fence)) {
        continue;
      }

      const auto acquire_ops = collectFenceWitnesses(acq_fence, false);
      if (acquire_ops.empty()) {
        continue;
      }

      bool synchronized = false;
      for (const Instruction *release_op : release_ops) {
        if (synchronized) {
          break;
        }
        for (const Instruction *acquire_op : acquire_ops) {
          if (!m_atomic_hb_pairs.count({release_op, acquire_op})) {
            continue;
          }

          SyncNode *rel_node = m_tfg->getNode(rel_fence);
          SyncNode *acq_node = m_tfg->getNode(acq_fence);
          if (rel_node && acq_node) {
            m_atomic_hb_pairs.insert({rel_fence, acq_fence});
            m_tfg->addInterThreadEdge(rel_node, acq_node);
            ++fence_pairs;
          }
          synchronized = true;
          break;
        }
      }
    }
  }

  errs() << "Added " << fence_pairs << " fence-based HB edges.\n";
}

void MHPAnalysis::computeSeqCstTotalOrder() {
  errs() << "Skipped synthetic seq-cst total-order edges.\n";
}

bool MHPAnalysis::mayRace(const Instruction *i1, const Instruction *i2) const {
  // Two instructions may race if:
  // 1. They may happen in parallel
  // 2. At least one is a write
  // 3. They access the same memory location
  // 4. They don't access thread-local storage

  // Check MHP
  if (!mayHappenInParallel(i1, i2)) {
    return false;
  }

  // Check if either accesses thread-local storage (can't race)
  if (m_tls_analysis) {
    if (m_tls_analysis->accessesThreadLocalStorage(i1) ||
        m_tls_analysis->accessesThreadLocalStorage(i2)) {
      return false;
    }
  }

  // Check if at least one is a write
  bool is_write1 = isa<StoreInst>(i1) || isa<AtomicRMWInst>(i1) ||
                   isa<AtomicCmpXchgInst>(i1);
  bool is_write2 = isa<StoreInst>(i2) || isa<AtomicRMWInst>(i2) ||
                   isa<AtomicCmpXchgInst>(i2);

  if (!is_write1 && !is_write2) {
    return false; // Read-read: no race
  }

  // Check if they access the same memory location (via alias analysis)
  const Value *ptr1 = nullptr;
  const Value *ptr2 = nullptr;

  if (const LoadInst *load = dyn_cast<LoadInst>(i1)) {
    ptr1 = load->getPointerOperand();
  } else if (const StoreInst *store = dyn_cast<StoreInst>(i1)) {
    ptr1 = store->getPointerOperand();
  } else if (const AtomicRMWInst *rmw = dyn_cast<AtomicRMWInst>(i1)) {
    ptr1 = rmw->getPointerOperand();
  } else if (const AtomicCmpXchgInst *cmpxchg =
                 dyn_cast<AtomicCmpXchgInst>(i1)) {
    ptr1 = cmpxchg->getPointerOperand();
  }

  if (const LoadInst *load = dyn_cast<LoadInst>(i2)) {
    ptr2 = load->getPointerOperand();
  } else if (const StoreInst *store = dyn_cast<StoreInst>(i2)) {
    ptr2 = store->getPointerOperand();
  } else if (const AtomicRMWInst *rmw = dyn_cast<AtomicRMWInst>(i2)) {
    ptr2 = rmw->getPointerOperand();
  } else if (const AtomicCmpXchgInst *cmpxchg =
                 dyn_cast<AtomicCmpXchgInst>(i2)) {
    ptr2 = cmpxchg->getPointerOperand();
  }

  if (!ptr1 || !ptr2) {
    return false; // Not memory operations
  }

  // Check for aliasing
  if (m_alias_analysis) {
    return m_alias_analysis->mayAlias(ptr1, ptr2);
  }

  // Conservative: assume may alias
  return true;
}

void MHPAnalysis::computeHappensBeforeTransitiveClosure() const {
  if (m_hb_closure_computed) {
    return; // Already computed
  }

  errs() << "Computing happens-before transitive closure...\n";

  // Collect all instructions that have TFG nodes
  std::vector<const Instruction *> all_insts;
  for (const auto &entry : m_inst_to_thread) {
    if (m_tfg->getNode(entry.first)) {
      all_insts.push_back(entry.first);
    }
  }

  // Initialize: each instruction can reach itself (reflexive)
  for (const Instruction *inst : all_insts) {
    m_hb_transitive_closure[inst].insert(inst);
  }

  // Add direct edges from TFG
  for (SyncNode *node : m_tfg->getAllNodes()) {
    const Instruction *from_inst = node->getInstruction();
    if (!from_inst)
      continue;

    for (SyncNode *succ : node->getSuccessors()) {
      const Instruction *to_inst = succ->getInstruction();
      if (!to_inst)
        continue;

      // Skip edges to thread-ambiguous instructions
      if (isInstructionThreadAmbiguous(to_inst)) {
        continue;
      }

      m_hb_transitive_closure[from_inst].insert(to_inst);
    }
  }

  // Floyd-Warshall-style transitive closure
  // For each instruction k, for each pair (i, j):
  //   if i->k and k->j, then add i->j
  //
  // This is O(N^3) but only needs to be done once and can be amortized
  // over many HB queries. For large programs, enable selectively.

  size_t num_insts = all_insts.size();
  if (num_insts > 10000) {
    errs() << "Warning: Large program (" << num_insts
           << " insts), skipping full transitive closure computation\n";
    m_hb_closure_computed = true;
    return;
  }

  for (const Instruction *k : all_insts) {
    auto k_it = m_hb_transitive_closure.find(k);
    if (k_it == m_hb_transitive_closure.end()) {
      continue;
    }

    const auto &k_reachable = k_it->second;

    for (const Instruction *i : all_insts) {
      // Check if i can reach k
      auto i_it = m_hb_transitive_closure.find(i);
      if (i_it == m_hb_transitive_closure.end()) {
        continue;
      }

      if (i_it->second.count(k) == 0) {
        continue; // i cannot reach k
      }

      // i can reach k; add all of k's reachable nodes to i's reachable set
      i_it->second.insert(k_reachable.begin(), k_reachable.end());
    }
  }

  m_hb_closure_computed = true;

  size_t total_edges = 0;
  for (const auto &entry : m_hb_transitive_closure) {
    total_edges += entry.second.size();
  }

  errs() << "Transitive closure computed: " << total_edges
         << " total reachability edges\n";
}
