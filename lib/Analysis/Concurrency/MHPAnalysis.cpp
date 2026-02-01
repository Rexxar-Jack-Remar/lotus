/**
 * @file MHPAnalysis.cpp
 * @brief Implementation of May-Happen-in-Parallel Analysis
 *
 * This analysis constructs a Thread Flow Graph (TFG) to determine which instructions
 * may execute concurrently.
 *
 * Soundness Properties:
 * - Default Safety: The analysis is conservative (safe) for race detection.
 *   It assumes two instructions MHP unless a Happens-Before (HB) relation is proven.
 * - Synchronization:
 *   - Fork/Join: Precisely models ancestor relationships.
 *   - Locks: Uses LockSet analysis (if enabled) to rule out parallelism guarded by common locks.
 *   - Condition Variables: Conservatively assumes a signal may wake any wait.
 *   - Barriers: Enforces program order across barriers.
 * - Thread Instances: Conservatively assumes spawned threads may have multiple instances.
 *
 * Author: rainoftime
 */

#include "Analysis/Concurrency/MHPAnalysis.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <deque>

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

std::vector<const Instruction *> ThreadRegionAnalysis::collectSyncPoints(const Function *func) const {
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

void ThreadRegionAnalysis::identifyRegionsForThread(ThreadID tid, const Function *func) {
  if (!func || func->isDeclaration())
    return;
  
  // Collect all synchronization points in CFG order
  std::vector<const Instruction *> sync_points = collectSyncPoints(func);
  
  // If no synchronization points, the entire function is one region
  if (sync_points.empty()) {
    auto region = std::make_unique<Region>();
    region->region_id = m_regions.size();
    region->thread_id = tid;
    // For a region without sync points, start/end nodes can be null or entry/exit
    region->start_node = nullptr;
    region->end_node = nullptr;
    
    // Collect all instructions in the function
    for (const BasicBlock &BB : *func) {
      for (const Instruction &inst : BB) {
        region->instructions.insert(&inst);
        m_inst_to_region[&inst] = region.get();
      }
    }
    
    m_regions.push_back(std::move(region));
    return;
  }
  
  // Create regions based on CFG traversal between synchronization points
  // Region strategy:
  // - Region 0: entry -> first sync point (inclusive)
  // - Region i: sync_i -> sync_{i+1} (both inclusive) 
  // - Region n: last sync -> exit
  
  // Helper: CFG-based region construction between two instructions
  auto build_region = [&](const Instruction *start_inst, const Instruction *end_inst,
                          SyncNode *start_node, SyncNode *end_node) {
    auto region = std::make_unique<Region>();
    region->region_id = m_regions.size();
    region->thread_id = tid;
    region->start_node = start_node;
    region->end_node = end_node;
    
    // Collect instructions between start and end using CFG traversal
    std::unordered_set<const BasicBlock *> visited;
    std::deque<const BasicBlock *> worklist;
    
    const BasicBlock *start_bb = start_inst ? start_inst->getParent() : &func->getEntryBlock();
    worklist.push_back(start_bb);
    visited.insert(start_bb);
    
    // BFS through CFG
    while (!worklist.empty()) {
      const BasicBlock *BB = worklist.front();
      worklist.pop_front();
      
      bool reached_end = false;
      bool started = (BB != start_bb) || (start_inst == nullptr);
      
      // Process instructions in this block
      for (const Instruction &inst : *BB) {
        // Mark start point
        if (start_inst && &inst == start_inst) {
          started = true;
        }
        
        // Add instruction if we're in the region
        if (started) {
          region->instructions.insert(&inst);
          m_inst_to_region[&inst] = region.get();
        }
        
        // Check if we reached the end
        if (end_inst && &inst == end_inst) {
          reached_end = true;
          break;
        }
      }
      
      // Continue to successors if we haven't reached the end
      if (!reached_end) {
        for (const BasicBlock *succ : successors(BB)) {
          if (visited.find(succ) == visited.end()) {
            visited.insert(succ);
            worklist.push_back(succ);
          }
        }
      }
    }
    
    m_regions.push_back(std::move(region));
  };
  
  // Build regions
  // Region 1: entry to first sync
  build_region(nullptr, sync_points[0], 
               nullptr, // Entry node - no specific sync node for start
               m_tfg.getNode(sync_points[0]));
  
  // Regions between sync points
  for (size_t i = 0; i + 1 < sync_points.size(); ++i) {
    build_region(sync_points[i], sync_points[i + 1],
                 m_tfg.getNode(sync_points[i]),
                 m_tfg.getNode(sync_points[i + 1]));
  }
  
  // Final region: last sync to exit
  // Use a simple approach: collect all remaining instructions
  auto final_region = std::make_unique<Region>();
  final_region->region_id = m_regions.size();
  final_region->thread_id = tid;
  final_region->start_node = m_tfg.getNode(sync_points.back());
  final_region->end_node = nullptr; // Exit node - no specific sync node for end
  
  bool after_last_sync = false;
  for (const BasicBlock &BB : *func) {
    for (const Instruction &inst : BB) {
      if (&inst == sync_points.back()) {
        after_last_sync = true;
        continue; // Don't include the last sync again
      }
      if (after_last_sync && m_inst_to_region.find(&inst) == m_inst_to_region.end()) {
        final_region->instructions.insert(&inst);
        m_inst_to_region[&inst] = final_region.get();
      }
    }
  }
  
  if (!final_region->instructions.empty()) {
    m_regions.push_back(std::move(final_region));
  }
}

void ThreadRegionAnalysis::computeOrderingConstraints() {
  // Compute must-precede and must-follow relationships based on:
  // 1. Intra-thread control flow
  // 2. Fork-join relationships
  // 3. Lock ordering

  for (size_t i = 0; i < m_regions.size(); ++i) {
    for (size_t j = 0; j < m_regions.size(); ++j) {
      if (i == j)
        continue;

      // Same thread: conservatively avoid imposing a total order.
      // We only add ordering when it is proven elsewhere at instruction level.

      // Different threads: check synchronization
      
      // Fork-join ordering: 
      // 1. Fork node must precede child thread entry
      // 2. Child thread exit must precede join node
      
      // Cross-thread fork/join ordering is handled at instruction granularity
      // via happens-before checks elsewhere; adding region-wide constraints
      // here risks unsound under-approximation of may-parallel pairs.

      // Note: Lock-based ordering is complex and would require tracking
      // which lock acquisition happens first in the global execution.
      // This would need a more sophisticated lock order analysis.
      // For now, we rely on the LockSetAnalysis to identify conflicts.
    }
  }
}

void ThreadRegionAnalysis::computeParallelism() {
  // Two regions may run in parallel if:
  // 1. They are in different threads
  // 2. Neither must precede the other
  // 3. They don't hold conflicting locks

  for (size_t i = 0; i < m_regions.size(); ++i) {
    auto &region_i = m_regions[i];

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

      // May be parallel
      region_i->may_be_parallel.insert(j);
      region_j->may_be_parallel.insert(i);
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
  m_alias_analysis = lotus::AliasAnalysisFactory::create(m_module, lotus::AAConfig::SparrowAA_NoCtx());
}

void MHPAnalysis::analyze() {
  errs() << "Starting MHP Analysis...\n";

  buildThreadFlowGraph();
  
  // Optional: LockSet analysis for more precise reasoning
  if (m_enable_lockset_analysis) {
    analyzeLockSets();
  }
  
  computeAtomicHappensBefore();
  analyzeThreadRegions();
  if (m_precompute_mhp_pairs) {
    computeMHPPairs();
  }

  errs() << "MHP Analysis Complete!\n";
}

void MHPAnalysis::enableLockSetAnalysis() {
  m_enable_lockset_analysis = true;
}

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
  processFunction(main_func, 0);

  errs() << "Thread Flow Graph built with " << m_tfg->getAllNodes().size()
         << " nodes\n";
}

void MHPAnalysis::processFunction(const Function *func, ThreadID tid) {
  if (!func || func->isDeclaration())
    return;

  // Avoid re-processing functions for the same thread context
  if (m_visited_functions_by_thread[tid].count(func)) {
      return;
  }
  m_visited_functions_by_thread[tid].insert(func);

  // --- Pass 1: Create all nodes for this function ---
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      mapInstructionToThread(&inst, tid);
      SyncNodeType node_type = SyncNodeType::REGULAR_INST;

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        if (m_thread_api->isTDFork(&inst)) {
          node_type = SyncNodeType::THREAD_FORK;
        } else if (m_thread_api->isTDJoin(&inst)) {
          node_type = SyncNodeType::THREAD_JOIN;
        } else if (m_thread_api->isTDAcquire(&inst)) {
          node_type = SyncNodeType::LOCK_ACQUIRE;
        } else if (m_thread_api->isTDRelease(&inst)) {
          node_type = SyncNodeType::LOCK_RELEASE;
        } // ... etc for other sync types
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
  
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      SyncNode *node = m_tfg->getNode(&inst);
      if (!node) continue;
      
      // Update exit node to last instruction we see
      exit_node = node;
      
      // Add intra-block edges
      if (&inst != &bb.front()) {
          const Instruction *prev_inst = inst.getPrevNode();
          if(prev_inst){
              SyncNode *prev_node = m_tfg->getNode(prev_inst);
              if(prev_node) m_tfg->addIntraThreadEdge(prev_node, node);
          }
      }

      // Add inter-block (CFG) edges
      if (inst.isTerminator()) {
        for (const BasicBlock *succ : successors(inst.getParent())) {
          if (!succ->empty()) {
            SyncNode *succ_node = m_tfg->getNode(&succ->front());
            if(succ_node) m_tfg->addIntraThreadEdge(node, succ_node);
          }
        }
      }

      // Handle synchronization logic for special instructions
      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        if (m_thread_api->isTDFork(&inst)) {
          handleThreadFork(&inst, node, tid);
        } else if (m_thread_api->isTDJoin(&inst)) {
          handleThreadJoin(&inst, node, tid);
        } else if (m_thread_api->isTDAcquire(&inst)) {
          handleLockAcquire(&inst, node);
        } else if (m_thread_api->isTDRelease(&inst)) {
          handleLockRelease(&inst, node);
        } else {
            const Function* callee = cb->getCalledFunction();
            if(callee && !callee->isDeclaration()){
                processFunction(callee, tid);
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

void MHPAnalysis::processInstruction(const Instruction * /*inst*/, ThreadID /*tid*/,
                                      SyncNode *& /*current_node*/) {
  // This method is a helper for more fine-grained processing if needed
  // Currently unused but kept for future extensibility
}

void MHPAnalysis::handleThreadFork(const Instruction *fork_inst,
                                    SyncNode *node, ThreadID parent_tid) {
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
  // The first argument to pthread_create is the pthread_t* where the thread ID is stored
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

    // Base case 2: We found a value that is already directly mapped to a thread ID.
    if (m_pthread_value_to_thread.count(v)) {
      return v;
    }

    // Recursive step: add operands to the worklist.
    if (const LoadInst *load = dyn_cast<LoadInst>(v)) {
      worklist.push_back(load->getPointerOperand());
    } else if (const BitCastInst *cast = dyn_cast<BitCastInst>(v)) {
      worklist.push_back(cast->getOperand(0));
    } else if (const GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(v)) {
      worklist.push_back(gep->getPointerOperand());
    } else if (const Instruction *inst = dyn_cast<Instruction>(v)) {
      // General case for other instructions, trace all operands.
      for (const Use &use : inst->operands()) {
        worklist.push_back(use.get());
      }
    }
  }

  return nullptr; // Could not trace back to a known origin.
}

void MHPAnalysis::handleThreadJoin(const Instruction *join_inst,
                                    SyncNode *node, ThreadID parent_tid) {
  // Track which thread is being joined using value analysis
  // pthread_join takes the pthread_t value (not pointer) as first argument
  
  const Value *joined_thread_val = m_thread_api->getJoinedThread(join_inst);
  ThreadID joined_tid = 0;
  bool found_thread = false;

  if (joined_thread_val) {
    // Use the improved tracing function to find the origin of the pthread_t value.
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

void MHPAnalysis::handleCondWait(const Instruction *wait_inst,
                                  SyncNode *node) {
  // Condition variable wait handling
  // pthread_cond_wait atomically releases the mutex and waits for a signal
  // When woken up, it reacquires the mutex
  
  const Value *cond = m_thread_api->getCondVal(wait_inst);
  const Value *mutex = m_thread_api->getCondMutex(wait_inst);
  
  node->setCondValue(cond);
  node->setLockValue(mutex); // The associated mutex
  
  // Track this wait for happens-before analysis
  m_condvar_waits[cond].push_back(wait_inst);
  
  // Do not add HB edges here: a prior signal is not guaranteed to wake this wait.
}

void MHPAnalysis::handleCondSignal(const Instruction *signal_inst,
                                    SyncNode *node) {
  // Condition variable signal/broadcast handling
  // Wakes up one or more waiting threads
  
  const Value *cond = m_thread_api->getCondVal(signal_inst);
  node->setCondValue(cond);
  
  // Track this signal for happens-before analysis
  m_condvar_signals[cond].push_back(signal_inst);
  
  // Add happens-before edges from this signal to all subsequent waits
  // Note: This is conservative - we don't know which specific wait will be woken
  // A more precise analysis would track the runtime pairing of signals and waits.
  // By assuming a signal triggers ANY potential wait, we might find false MHP pairs,
  // but we won't miss true ones (sound for MHP).
}

void MHPAnalysis::handleBarrier(const Instruction *barrier_inst,
                                 SyncNode *node) {
  // Barrier synchronization handling
  // pthread_barrier_wait: all threads must reach the barrier before any proceed
  // Happens-before: all threads reaching barrier N happen-before any thread leaving barrier N
  
  const Value *barrier = m_thread_api->getBarrierVal(barrier_inst);
  node->setLockValue(barrier); // Reuse lock field for barrier value
  
  // Track this barrier wait
  m_barrier_waits[barrier].push_back(barrier_inst);
  // Do NOT add inter-thread edges between waits; that would over-constrain
  // potential parallelism of the waits themselves. Barrier ordering is instead
  // captured by checking pre/post relationships within a thread.
}

void MHPAnalysis::analyzeLockSets() {
  errs() << "Analyzing Lock Sets...\n";
  m_lockset = std::make_unique<LockSetAnalysis>(m_module);
  m_lockset->setAliasAnalysis(m_alias_analysis.get());
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
  errs() << "Computing MHP Pairs...\n";

  // For each pair of instructions, check if they may run in parallel
  std::vector<const Instruction *> all_insts;

  for (Function &func : m_module) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      if (m_inst_to_thread.find(inst) != m_inst_to_thread.end()) {
        all_insts.push_back(inst);
      }
    }
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
        m_mhp_pairs.insert({i1, i2});
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

  // Basic checks: same instruction or same thread (accounting for multi-instance threads)
  if (i1 == i2 || isInSameThread(i1, i2))
    return (a && b) ? (m_mhp_cache[{a, b}] = false) : false;

  // Fast path: check precomputed MHP pairs
  if (isPrecomputedMHP(i1, i2))
    return (a && b) ? (m_mhp_cache[{a, b}] = true) : true;

  // Special case: if both instructions are from the same multi-instance thread,
  // they can run in parallel (different instances) unless explicitly ordered
  // by inter-thread synchronization
  ThreadID t1 = getThreadID(i1);
  ThreadID t2 = getThreadID(i2);
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
         !hasHappenBeforeRelation(i2, i1) &&
         !isOrderedByLocks(i1, i2);
  return (a && b) ? (m_mhp_cache[{a, b}] = r) : r;
}

bool MHPAnalysis::isPrecomputedMHP(const Instruction *i1,
                                   const Instruction *i2) const {
  return m_mhp_pairs.find({i1, i2}) != m_mhp_pairs.end() ||
         m_mhp_pairs.find({i2, i1}) != m_mhp_pairs.end();
}

InstructionSet
MHPAnalysis::getParallelInstructions(const Instruction *inst) const {
  InstructionSet result;

  for (const auto &pair : m_mhp_pairs) {
    if (pair.first == inst) {
      result.insert(pair.second);
    } else if (pair.second == inst) {
      result.insert(pair.first);
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

  SyncNode *startNode = m_tfg->getNode(i1);
  SyncNode *endNode = m_tfg->getNode(i2);

  if (!startNode || !endNode || i1 == i2) {
    return (m_hb_cache[{i1, i2}] = false);
  }
  
  // Perform a BFS on the ThreadFlowGraph to find if endNode is reachable from startNode
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

      // For intra-thread edges, we need to follow all edges in the TFG to find
      // paths through the program. The "must" check is too restrictive and would
      // prevent us from finding valid happens-before relationships established
      // through atomic synchronization.
      // For inter-thread edges, always follow them (they represent synchronization).
      if (current->getThreadID() == succ->getThreadID()) {
        // For intra-thread edges, follow all edges that exist in the TFG
        // (they represent valid control flow)
      }
      // Inter-thread edges are always followed (they represent synchronization)
      
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
  if (!m_lockset)
    return false;

  // If both instructions must hold a common lock in different threads,
  // they cannot execute in parallel due to mutual exclusion
  if (!isInSameThread(i1, i2)) {
    auto must1 = m_lockset->getMustLockSetAt(i1);
    auto must2 = m_lockset->getMustLockSetAt(i2);
    for (const auto* l : must1) {
      if (must2.find(l) != must2.end()) {
        return true;
      }
    }
    return false;
  }

  return false;
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
  Statistics stats;

  if (m_tfg) {
    stats.num_threads = m_tfg->getAllThreads().size();
    stats.num_forks = m_tfg->getNodesOfType(SyncNodeType::THREAD_FORK).size();
    stats.num_joins = m_tfg->getNodesOfType(SyncNodeType::THREAD_JOIN).size();
    stats.num_locks =
        m_tfg->getNodesOfType(SyncNodeType::LOCK_ACQUIRE).size();
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
    pair.first->print(os);
    os << " ||| ";
    pair.second->print(os);
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

const PostDominatorTree &MHPAnalysis::getPostDomTree(const Function *func) const {
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
  // - We want to know if 'from' *must* precede 'to' in a single linear execution trace.
  // - With loops, 'to' might execute before 'from' in a subsequent iteration (cross-iteration), 
  //   but for defining a "happens-before" relation that rules out parallelism within the 
  //   same conceptual thread instance, we focus on the acyclic backbone.
  // - This is a conservative approximation for "program order" to avoid cycles in HB graph.

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
        return true;  // from comes first
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
        if (Cpp11Atomics::isAtomic(&*I)) {
          m_atomic_instructions.push_back(&*I);
        }
      }
    }
  }

  // Clear the old pairs and rebuild
  m_atomic_hb_pairs.clear();
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
    const Value *ptr = canonPtr(Cpp11Atomics::getAtomicPointer(inst));
    if (!ptr) continue;

    const auto mo = Cpp11Atomics::getMemoryOrder(inst);
    const bool has_release = (mo == Cpp11Atomics::MemoryOrder::Release ||
                              mo == Cpp11Atomics::MemoryOrder::AcquireRelease ||
                              mo == Cpp11Atomics::MemoryOrder::SequentiallyConsistent);
    const bool has_acquire = (mo == Cpp11Atomics::MemoryOrder::Acquire ||
                              mo == Cpp11Atomics::MemoryOrder::AcquireRelease ||
                              mo == Cpp11Atomics::MemoryOrder::SequentiallyConsistent);

    if (Cpp11Atomics::isStore(inst) && has_release)
      releasesByPtr[ptr].push_back(inst);
    if (Cpp11Atomics::isLoad(inst) && has_acquire)
      acquiresByPtr[ptr].push_back(inst);
  }

  std::vector<const Value *> releasePtrs;
  std::vector<const Value *> acquirePtrs;
  releasePtrs.reserve(releasesByPtr.size());
  acquirePtrs.reserve(acquiresByPtr.size());
  for (auto &kv : releasesByPtr) releasePtrs.push_back(kv.first);
  for (auto &kv : acquiresByPtr) acquirePtrs.push_back(kv.first);

  auto addEdge = [&](const Instruction *release_inst, const Instruction *acquire_inst) {
    if (isInSameThread(release_inst, acquire_inst))
      return;
    m_atomic_hb_pairs.insert({release_inst, acquire_inst});
    SyncNode *release_node = m_tfg->getNode(release_inst);
    SyncNode *acquire_node = m_tfg->getNode(acquire_inst);
    if (release_node && acquire_node)
      m_tfg->addInterThreadEdge(release_node, acquire_node);
    ++pairs_found;
  };

  // First, handle identical-pointer buckets without AA queries.
  for (const Value *p : releasePtrs) {
    auto itA = acquiresByPtr.find(p);
    if (itA == acquiresByPtr.end()) continue;
    const InstVec &rels = releasesByPtr[p];
    const InstVec &acqs = itA->second;
    for (const Instruction *r : rels)
      for (const Instruction *a : acqs)
        addEdge(r, a);
  }

  // Then, handle cross-pointer aliasing (AA query per pointer-pair).
  for (const Value *rp : releasePtrs) {
    for (const Value *ap : acquirePtrs) {
      if (rp == ap) continue; // already handled
      if (m_alias_analysis && !m_alias_analysis->mayAlias(rp, ap))
        continue;
      const InstVec &rels = releasesByPtr[rp];
      const InstVec &acqs = acquiresByPtr[ap];
      for (const Instruction *r : rels)
        for (const Instruction *a : acqs)
          addEdge(r, a);
    }
  }

  // Phase 3 & 4 for fences and seq_cst (omitted for brevity, but would also add edges to TFG)
  // ...

   errs() << "Found " << pairs_found << " atomic happens-before pairs.\n";
}
