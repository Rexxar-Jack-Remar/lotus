/**
 * @file LockSetAnalysis.cpp
 * @brief Implementation of Lock Set Analysis
 *
 * This analysis tracks the set of locks held at each program point.
 * It computes two sets for each instruction:
 * 1. May-Lock Set: Locks that MIGHT be held. Used for deadlock detection and reducing false positives.
 *    - Join operator: Union
 *    - Try-lock: Assumed successful
 * 2. Must-Lock Set: Locks that MUST be held. Used for proving mutual exclusion (safety).
 *    - Join operator: Intersection
 *    - Try-lock: Assumed failed (safe approximation)
 *    - Release: Removes all aliasing locks to ensure soundness.
 */

#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <queue>
#include <stack>

using namespace llvm;
using namespace mhp;

// ============================================================================
// Construction and Analysis
// ============================================================================

LockSetAnalysis::LockSetAnalysis(Module &module)
    : m_module(&module), m_single_function(nullptr),
      m_thread_api(ThreadAPI::getThreadAPI()), m_alias_analysis(nullptr),
      m_call_graph(nullptr) {}

LockSetAnalysis::LockSetAnalysis(Function &func)
    : m_module(nullptr), m_single_function(&func),
      m_thread_api(ThreadAPI::getThreadAPI()), m_alias_analysis(nullptr),
      m_call_graph(nullptr) {}

void LockSetAnalysis::analyze() {
  errs() << "Starting Lock Set Analysis...\n";

  // Identify all locks in the program
  identifyLocks();

  if (m_module) {
    // Module-wide analysis
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        analyzeFunction(&func);
      }
    }
    computeInterproceduralLockSets();
  } else if (m_single_function) {
    // Single function analysis
    analyzeFunction(m_single_function);
  }

  // Track lock ordering for deadlock detection
  trackLockOrdering();

  errs() << "Lock Set Analysis Complete!\n";
  errs() << "Found " << m_all_locks.size() << " locks\n";
}

// ============================================================================
// Query Interface
// ============================================================================

LockSet LockSetAnalysis::getMayLockSetAt(const Instruction *inst) const {
  auto it = m_may_locksets_entry.find(inst);
  if (it != m_may_locksets_entry.end())
    return it->second;
  return LockSet();
}

LockSet LockSetAnalysis::getMayReadLockSetAt(const Instruction *inst) const {
  auto it = m_may_read_locks_entry.find(inst);
  if (it != m_may_read_locks_entry.end())
    return it->second;
  return LockSet();
}

LockSet LockSetAnalysis::getMayWriteLockSetAt(const Instruction *inst) const {
  auto it = m_may_write_locks_entry.find(inst);
  if (it != m_may_write_locks_entry.end())
    return it->second;
  return LockSet();
}

LockSet LockSetAnalysis::getMustLockSetAt(const Instruction *inst) const {
  auto it = m_must_locksets_entry.find(inst);
  if (it != m_must_locksets_entry.end())
    return it->second;
  return LockSet();
}

LockSet LockSetAnalysis::getMustReadLockSetAt(const Instruction *inst) const {
  auto it = m_must_read_locks_entry.find(inst);
  if (it != m_must_read_locks_entry.end())
    return it->second;
  return LockSet();
}

LockSet LockSetAnalysis::getMustWriteLockSetAt(const Instruction *inst) const {
  auto it = m_must_write_locks_entry.find(inst);
  if (it != m_must_write_locks_entry.end())
    return it->second;
  return LockSet();
}

bool LockSetAnalysis::mayHoldLock(const Instruction *inst, LockID lock) const {
  auto lockset = getMayLockSetAt(inst);
  for (const auto *held_lock : lockset) {
    if (mayAlias(lock, held_lock)) {
      return true;
    }
  }
  return false;
}

bool LockSetAnalysis::mustHoldLock(const Instruction *inst, LockID lock) const {
  auto lockset = getMustLockSetAt(inst);
  return lockset.find(lock) != lockset.end();
}

std::unordered_set<const Instruction *>
LockSetAnalysis::getInstructionsHoldingLock(LockID lock) const {
  std::unordered_set<const Instruction *> result;
  for (const auto &pair : m_may_locksets_entry) {
    if (pair.second.find(lock) != pair.second.end()) {
      result.insert(pair.first);
    }
  }
  return result;
}

bool LockSetAnalysis::mayHoldCommonLock(const Instruction *i1,
                                         const Instruction *i2) const {
  auto common = [this](const LockSet &a, const LockSet &b) {
    for (const auto *lock : a) {
      if (b.find(lock) != b.end()) return true;
      for (const auto *lock2 : b) {
        if (mayAlias(lock, lock2)) return true;
      }
    }
    return false;
  };
  LockSet r1 = getMayReadLockSetAt(i1), r2 = getMayReadLockSetAt(i2);
  LockSet w1 = getMayWriteLockSetAt(i1), w2 = getMayWriteLockSetAt(i2);
  return common(w1, w2) || common(r1, r2);
}

LockSet LockSetAnalysis::getAllLocksInFunction(const Function *func) const {
  LockSet all_locks;
  for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
       ++I) {
    const Instruction *inst = &*I;
    if (isLockOperation(inst)) {
      LockID lock = getLockValue(inst);
      if (lock) {
        all_locks.insert(lock);
      }
    }
  }
  return all_locks;
}

std::vector<const Instruction *>
LockSetAnalysis::getLockAcquires(LockID lock) const {
  auto it = m_lock_acquires.find(lock);
  if (it != m_lock_acquires.end()) {
    return it->second;
  }
  return std::vector<const Instruction *>();
}

std::vector<const Instruction *>
LockSetAnalysis::getLockReleases(LockID lock) const {
  auto it = m_lock_releases.find(lock);
  if (it != m_lock_releases.end()) {
    return it->second;
  }
  return std::vector<const Instruction *>();
}

// ============================================================================
// Advanced Queries
// ============================================================================

bool LockSetAnalysis::isReentrantLock(LockID lock) const {
  return m_reentrant_locks.find(lock) != m_reentrant_locks.end();
}

size_t LockSetAnalysis::getLockNestingDepth(const Instruction *inst) const {
  return getMayLockSetAt(inst).size();
}

bool LockSetAnalysis::areLocksOrderedConsistently(LockID lock1,
                                                   LockID lock2) const {
  bool found_12 = m_observed_lock_orders.find({lock1, lock2}) !=
                  m_observed_lock_orders.end();
  bool found_21 = m_observed_lock_orders.find({lock2, lock1}) !=
                  m_observed_lock_orders.end();

  // Consistent if only one order is observed
  return !(found_12 && found_21);
}

std::vector<std::pair<LockID, LockID>>
LockSetAnalysis::detectLockOrderInversions() const {
  std::vector<std::pair<LockID, LockID>> inversions;

  // Check all pairs of locks for order inversions
  for (const auto &pair1 : m_observed_lock_orders) {
    LockPair reverse{pair1.second, pair1.first};
    if (m_observed_lock_orders.find(reverse) != m_observed_lock_orders.end()) {
      // Found an inversion - both lock1->lock2 and lock2->lock1 exist
      inversions.push_back({pair1.first, pair1.second});
    }
  }

  return inversions;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

void LockSetAnalysis::Statistics::print(raw_ostream &os) const {
  os << "Lock Set Analysis Statistics:\n";
  os << "==============================\n";
  os << "Locks:                " << num_locks << "\n";
  os << "Lock Acquires:        " << num_acquires << "\n";
  os << "Lock Releases:        " << num_releases << "\n";
  os << "Try-Lock Operations:  " << num_try_acquires << "\n";
  os << "Max Depth:     " << max_nesting_depth << "\n";
  os << "Observed Reentrant Locks:     " << num_reentrant_locks << "\n";
  os << "Potential Deadlocks:  " << num_potential_deadlocks << "\n";
}

LockSetAnalysis::Statistics LockSetAnalysis::getStatistics() const {
  Statistics stats;

  stats.num_locks = m_all_locks.size();

  stats.num_acquires = 0;
  for (const auto &pair : m_lock_acquires) {
    stats.num_acquires += pair.second.size();
  }

  stats.num_releases = 0;
  for (const auto &pair : m_lock_releases) {
    stats.num_releases += pair.second.size();
  }

  stats.num_try_acquires = 0;
  for (const auto &pair : m_lock_try_acquires) {
    stats.num_try_acquires += pair.second.size();
  }

  stats.max_nesting_depth = 0;
  for (const auto &pair : m_may_locksets_entry) {
    stats.max_nesting_depth =
        std::max(stats.max_nesting_depth, pair.second.size());
  }

  stats.num_reentrant_locks = m_reentrant_locks.size();
  stats.num_potential_deadlocks = detectLockOrderInversions().size();

  return stats;
}

void LockSetAnalysis::printStatistics(raw_ostream &os) const {
  auto stats = getStatistics();
  stats.print(os);
}

void LockSetAnalysis::printResults(raw_ostream &os) const {
  os << "\n=== Lock Set Analysis Results ===\n\n";

  printStatistics(os);

  os << "\n=== All Locks ===\n";
  for (const auto *lock : m_all_locks) {
    os << "Lock: ";
    lock->printAsOperand(os, false);
    os << "\n";

    auto acquires = getLockAcquires(lock);
    os << "  Acquires: " << acquires.size() << "\n";

    auto releases = getLockReleases(lock);
    os << "  Releases: " << releases.size() << "\n";

    if (isReentrantLock(lock)) {
      os << "  [REENTRANT]\n";
    }
  }

  // Print potential deadlocks
  auto inversions = detectLockOrderInversions();
  if (!inversions.empty()) {
    os << "\n=== Potential Deadlocks (Lock Order Inversions) ===\n";
    for (const auto &pair : inversions) {
      os << "Lock ";
      pair.first->printAsOperand(os, false);
      os << " and Lock ";
      pair.second->printAsOperand(os, false);
      os << "\n";
    }
  }
}

void LockSetAnalysis::printLockSetsForFunction(const Function *func,
                                                raw_ostream &os) const {
  os << "Lock Sets for Function: " << func->getName() << "\n";
  os << "=============================================\n";

  for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
       ++I) {
    const Instruction *inst = &*I;

    auto may_locks = getMayLockSetAt(inst);
    auto must_locks = getMustLockSetAt(inst);

    if (!may_locks.empty() || isLockOperation(inst)) {
      os << "Instruction: ";
      inst->print(os);
      os << "\n";

      os << "  May-Locks: {";
      bool first = true;
      for (const auto *lock : may_locks) {
        if (!first)
          os << ", ";
        lock->printAsOperand(os, false);
        first = false;
      }
      os << "}\n";

      os << "  Must-Locks: {";
      first = true;
      for (const auto *lock : must_locks) {
        if (!first)
          os << ", ";
        lock->printAsOperand(os, false);
        first = false;
      }
      os << "}\n\n";
    }
  }
}

void LockSetAnalysis::print(raw_ostream &os) const {
  printResults(os);
}

// ============================================================================
// Visualization
// ============================================================================

void LockSetAnalysis::dumpLockGraph(const std::string &filename) const {
  std::error_code EC;
  raw_fd_ostream file(filename, EC, sys::fs::OF_None);

  if (EC) {
    errs() << "Error opening file " << filename << ": " << EC.message() << "\n";
    return;
  }

  file << "digraph LockGraph {\n";
  file << "  rankdir=LR;\n";
  file << "  node [shape=box];\n\n";

  // Create nodes for locks
  size_t id = 0;
  std::unordered_map<LockID, size_t> lock_ids;
  for (const auto *lock : m_all_locks) {
    lock_ids[lock] = id;
    file << "  lock" << id << " [label=\"";
    lock->printAsOperand(file, false);
    file << "\"];\n";
    id++;
  }

  file << "\n";

  // Create edges for lock ordering
  for (const auto &pair : m_observed_lock_orders) {
    auto it1 = lock_ids.find(pair.first);
    auto it2 = lock_ids.find(pair.second);
    if (it1 != lock_ids.end() && it2 != lock_ids.end()) {
      file << "  lock" << it1->second << " -> lock" << it2->second;

      // Highlight inversions in red
      LockPair reverse{pair.second, pair.first};
      if (m_observed_lock_orders.find(reverse) !=
          m_observed_lock_orders.end()) {
        file << " [color=red, style=bold]";
      }

      file << ";\n";
    }
  }

  file << "}\n";
  file.close();

  errs() << "Lock graph dumped to " << filename << "\n";
}

// ============================================================================
// Analysis Implementation
// ============================================================================

void LockSetAnalysis::analyzeFunction(Function *func) {
  if (!func || func->isDeclaration())
    return;

  computeIntraproceduralLockSets(func);
}

void LockSetAnalysis::computeIntraproceduralLockSets(Function *func) {
  // Standard forward dataflow analysis using a worklist algorithm.
  //
  // Lattice: Sets of LockIDs.
  // - May-Analysis: Union (Join).
  //   Effect: Collects all locks that *might* be held on any path to this point.
  // - Must-Analysis: Intersection (Join).
  //   Effect: Collects all locks that *must* be held on all paths to this point.
  
  // Worklist algorithm for dataflow analysis
  std::queue<const Instruction *> worklist;
  std::set<const Instruction *> in_worklist;

  const Instruction *entry = &func->getEntryBlock().front();
  m_may_locksets_entry[entry] = LockSet();
  m_must_locksets_entry[entry] = LockSet();
  m_may_read_locks_entry[entry] = LockSet();
  m_may_read_locks_exit[entry] = LockSet();
  m_may_write_locks_entry[entry] = LockSet();
  m_may_write_locks_exit[entry] = LockSet();
  m_must_read_locks_entry[entry] = LockSet();
  m_must_read_locks_exit[entry] = LockSet();
  m_must_write_locks_entry[entry] = LockSet();
  m_must_write_locks_exit[entry] = LockSet();
  worklist.push(entry);
  in_worklist.insert(entry);

  while (!worklist.empty()) {
    const Instruction *inst = worklist.front();
    worklist.pop();
    in_worklist.erase(inst);

    std::vector<LockSet> may_inputs, must_inputs;
    std::vector<LockSet> may_read_inputs, may_write_inputs;
    std::vector<LockSet> must_read_inputs, must_write_inputs;

    if (inst == entry) {
      may_inputs.push_back(LockSet());
      must_inputs.push_back(LockSet());
      may_read_inputs.push_back(LockSet());
      may_write_inputs.push_back(LockSet());
      must_read_inputs.push_back(LockSet());
      must_write_inputs.push_back(LockSet());
    } else {
      const BasicBlock *bb = inst->getParent();
      if (inst == &bb->front()) {
        for (const BasicBlock *pred : predecessors(bb)) {
          const Instruction *pred_term = pred->getTerminator();
          if (pred_term) {
            auto it_may = m_may_locksets_exit.find(pred_term);
            auto it_must = m_must_locksets_exit.find(pred_term);
            auto it_mr = m_may_read_locks_exit.find(pred_term);
            auto it_mw = m_may_write_locks_exit.find(pred_term);
            auto it_ur = m_must_read_locks_exit.find(pred_term);
            auto it_uw = m_must_write_locks_exit.find(pred_term);
            if (it_may != m_may_locksets_exit.end()) may_inputs.push_back(it_may->second);
            if (it_must != m_must_locksets_exit.end()) must_inputs.push_back(it_must->second);
            if (it_mr != m_may_read_locks_exit.end()) may_read_inputs.push_back(it_mr->second);
            if (it_mw != m_may_write_locks_exit.end()) may_write_inputs.push_back(it_mw->second);
            if (it_ur != m_must_read_locks_exit.end()) must_read_inputs.push_back(it_ur->second);
            if (it_uw != m_must_write_locks_exit.end()) must_write_inputs.push_back(it_uw->second);
          }
        }
      } else {
        const Instruction *prev = inst->getPrevNode();
        auto it_may = m_may_locksets_exit.find(prev);
        auto it_must = m_must_locksets_exit.find(prev);
        auto it_mr = m_may_read_locks_exit.find(prev);
        auto it_mw = m_may_write_locks_exit.find(prev);
        auto it_ur = m_must_read_locks_exit.find(prev);
        auto it_uw = m_must_write_locks_exit.find(prev);
        if (it_may != m_may_locksets_exit.end()) may_inputs.push_back(it_may->second);
        if (it_must != m_must_locksets_exit.end()) must_inputs.push_back(it_must->second);
        if (it_mr != m_may_read_locks_exit.end()) may_read_inputs.push_back(it_mr->second);
        if (it_mw != m_may_write_locks_exit.end()) may_write_inputs.push_back(it_mw->second);
        if (it_ur != m_must_read_locks_exit.end()) must_read_inputs.push_back(it_ur->second);
        if (it_uw != m_must_write_locks_exit.end()) must_write_inputs.push_back(it_uw->second);
      }
    }

    LockSet may_read_in = may_read_inputs.empty() ? LockSet() : merge(may_read_inputs, false);
    LockSet may_write_in = may_write_inputs.empty() ? LockSet() : merge(may_write_inputs, false);
    LockSet must_read_in = must_read_inputs.empty() ? LockSet() : merge(must_read_inputs, true);
    LockSet must_write_in = must_write_inputs.empty() ? LockSet() : merge(must_write_inputs, true);
    LockSet may_in = may_read_in;
    may_in.insert(may_write_in.begin(), may_write_in.end());
    LockSet must_in = must_read_in;
    must_in.insert(must_write_in.begin(), must_write_in.end());

    LockSet may_read_out, may_write_out, must_read_out, must_write_out;
    transferReadWrite(inst, may_read_in, may_write_in, may_read_out, may_write_out, false);
    transferReadWrite(inst, must_read_in, must_write_in, must_read_out, must_write_out, true);

    LockSet may_out = transfer(inst, may_in, false);
    LockSet must_out = transfer(inst, must_in, true);

    bool changed = false;
    if (m_may_read_locks_entry[inst] != may_read_in) { m_may_read_locks_entry[inst] = may_read_in; changed = true; }
    if (m_may_read_locks_exit[inst] != may_read_out) { m_may_read_locks_exit[inst] = may_read_out; changed = true; }
    if (m_may_write_locks_entry[inst] != may_write_in) { m_may_write_locks_entry[inst] = may_write_in; changed = true; }
    if (m_may_write_locks_exit[inst] != may_write_out) { m_may_write_locks_exit[inst] = may_write_out; changed = true; }
    if (m_must_read_locks_entry[inst] != must_read_in) { m_must_read_locks_entry[inst] = must_read_in; changed = true; }
    if (m_must_read_locks_exit[inst] != must_read_out) { m_must_read_locks_exit[inst] = must_read_out; changed = true; }
    if (m_must_write_locks_entry[inst] != must_write_in) { m_must_write_locks_entry[inst] = must_write_in; changed = true; }
    if (m_must_write_locks_exit[inst] != must_write_out) { m_must_write_locks_exit[inst] = must_write_out; changed = true; }
    if (m_may_locksets_entry[inst] != may_in) { m_may_locksets_entry[inst] = may_in; changed = true; }
    if (m_may_locksets_exit[inst] != may_out) { m_may_locksets_exit[inst] = may_out; changed = true; }
    if (m_must_locksets_entry[inst] != must_in) { m_must_locksets_entry[inst] = must_in; changed = true; }
    if (m_must_locksets_exit[inst] != must_out) { m_must_locksets_exit[inst] = must_out; changed = true; }

    // Add successors to worklist if changed
    if (changed) {
      // Add next instruction in block
      if (inst->getNextNode()) {
        const Instruction *next = inst->getNextNode();
        if (in_worklist.find(next) == in_worklist.end()) {
          worklist.push(next);
          in_worklist.insert(next);
        }
      } else if (inst->isTerminator()) {
        // Add successor blocks
        const BasicBlock *bb = inst->getParent();
        for (const BasicBlock *succ_bb : successors(bb)) {
          const Instruction *succ = &succ_bb->front();
          if (in_worklist.find(succ) == in_worklist.end()) {
            worklist.push(succ);
            in_worklist.insert(succ);
          }
        }
      }
    }
  }
}

void LockSetAnalysis::computeInterproceduralLockSets() {
  if (!m_call_graph) {
    errs() << "Warning: CallGraph not set. Skipping interprocedural analysis.\n";
    return;
  }

  errs() << "Computing interprocedural lock sets using CallGraph...\n";
  
  // Perform bottom-up traversal of call graph to compute function summaries
  bottomUpTraversal();
  
  // Re-analyze each function with interprocedural context
  for (Function &func : *m_module) {
    if (!func.isDeclaration()) {
      analyzeFunction(&func);
    }
  }
  
  errs() << "Interprocedural lock set analysis complete.\n";
}

LockSet LockSetAnalysis::transfer(const Instruction *inst,
                                   const LockSet &in_set, bool is_must) const {
  LockSet out_set = in_set;

  // Check if this is a lock operation
  if (m_thread_api->isTDAcquire(inst)) {
    // Try-lock may fail; without checking return value we don't add to either set
    if (!m_thread_api->isTryLock(inst)) {
      LockID lock = getLockValue(inst);
      if (lock) {
        out_set.insert(lock);
      }
    }
  } else if (m_thread_api->isTDRelease(inst)) {
    // Lock release - remove lock from set
    LockID lock = getLockValue(inst);
    if (lock) {
      out_set.erase(lock);

      // Only in must-analysis do we safely drop aliasing locks.
      // In may-analysis this would under-approximate the held set.
      // For Must-Lock analysis: If we release 'lock', we must also remove any lock 'l'
      // that *might* be an alias of 'lock'. If we kept 'l', we might falsely believe
      // we still hold 'l' when we actually released it via 'lock'.
      if (is_must && m_alias_analysis) {
        LockSet to_remove;
        for (const auto *l : out_set) {
          if (mayAlias(l, lock)) {
            to_remove.insert(l);
          }
        }
        for (const auto *l : to_remove) {
          out_set.erase(l);
        }
      }
    }
  } else if (const CallInst *call = dyn_cast<CallInst>(inst)) {
    // Try-lock is handled above (not added to set). Handle other calls.
    if (!m_thread_api->isTDAcquire(call) && !m_thread_api->isTDRelease(call)) {
      // Handle regular function calls with interprocedural summaries
      if (m_call_graph) {
        auto callees = getCallees(call);
        for (Function *callee : callees) {
          if (callee && !callee->isDeclaration()) {
            // Keep may- and must-lock flows independent to preserve soundness.
            if (!is_must) {
              LockSet may_only = out_set;
              LockSet must_dummy = out_set;
              applyFunctionSummary(call, callee, may_only, must_dummy);
              out_set = std::move(may_only);
            } else {
              LockSet may_dummy = out_set;
              LockSet must_only = out_set;
              applyFunctionSummary(call, callee, may_dummy, must_only);
              out_set = std::move(must_only);
            }
          }
        }
      }
    }
  }

  return out_set;
}

void LockSetAnalysis::transferReadWrite(const Instruction *inst,
                                         const LockSet &in_read,
                                         const LockSet &in_write,
                                         LockSet &out_read, LockSet &out_write,
                                         bool is_must) const {
  out_read = in_read;
  out_write = in_write;

  if (m_thread_api->isReadLockAcquire(inst)) {
    LockID lock = getLockValue(inst);
    if (lock) out_read.insert(lock);
  } else if (m_thread_api->isWriteLockAcquire(inst)) {
    LockID lock = getLockValue(inst);
    if (lock) {
      out_write.insert(lock);
      for (const auto *l : in_read) {
        if (mayAlias(l, lock)) out_read.erase(l);
      }
    }
  } else if (m_thread_api->isTDAcquire(inst)) {
    // Try-lock may fail; don't add to write set
    if (!m_thread_api->isTryLock(inst)) {
      LockID lock = getLockValue(inst);
      if (lock) out_write.insert(lock);
    }
  } else if (m_thread_api->isTDRelease(inst)) {
    LockID lock = getLockValue(inst);
    if (lock) {
      out_read.erase(lock);
      out_write.erase(lock);
      if (is_must && m_alias_analysis) {
        LockSet to_remove_r, to_remove_w;
        for (const auto *l : out_read) {
          if (mayAlias(l, lock)) to_remove_r.insert(l);
        }
        for (const auto *l : out_write) {
          if (mayAlias(l, lock)) to_remove_w.insert(l);
        }
        for (const auto *l : to_remove_r) out_read.erase(l);
        for (const auto *l : to_remove_w) out_write.erase(l);
      }
    }
  }
}

LockSet LockSetAnalysis::merge(const std::vector<LockSet> &sets,
                                bool is_must) const {
  if (sets.empty()) {
    return LockSet();
  }

  if (is_must) {
    // Must-analysis: intersection
    LockSet result = sets[0];
    for (size_t i = 1; i < sets.size(); ++i) {
      LockSet intersection;
      std::set_intersection(result.begin(), result.end(), sets[i].begin(),
                            sets[i].end(),
                            std::inserter(intersection, intersection.begin()));
      result = intersection;
    }
    return result;
  }      // May-analysis: union
    LockSet result;
    for (const auto &set : sets) {
      result.insert(set.begin(), set.end());
    }
    return result;
 
}

void LockSetAnalysis::identifyLocks() {
  auto process_func = [this](Function &func) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      Instruction *inst = &*I;

      if (m_thread_api->isTDAcquire(inst)) {
        LockID lock = getLockValue(inst);
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_acquires[lock].push_back(inst);

          // Check for try-lock
          if (const CallInst *call = dyn_cast<CallInst>(inst)) {
            if (call->getCalledFunction() &&
                call->getCalledFunction()->getName().contains("trylock")) {
              m_lock_try_acquires[lock].push_back(inst);
            }
          }
        }
      } else if (m_thread_api->isTDRelease(inst)) {
        LockID lock = getLockValue(inst);
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_releases[lock].push_back(inst);
        }
      }
    }
  };

  if (m_module) {
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        process_func(func);
      }
    }
  } else if (m_single_function) {
    process_func(*m_single_function);
  }
}

void LockSetAnalysis::trackLockOrdering() {
  // Track the order in which locks are acquired
  for (const auto &pair : m_may_locksets_entry) {
    const Instruction *inst = pair.first;
    const LockSet &locks_held = pair.second;

    // If this instruction acquires a new lock, record the ordering
    if (m_thread_api->isTDAcquire(inst)) {
      LockID new_lock = getLockValue(inst);
      if (new_lock) {
        // Check if lock is already in the lockset (reentrant)
        if (locks_held.find(new_lock) == locks_held.end()) {
          // Not reentrant
        } else {
          // Reentrant lock
          m_reentrant_locks.insert(new_lock);
        }

        // Record ordering with all currently held locks
        for (const auto *held_lock : locks_held) {
          if (held_lock != new_lock) {
            m_observed_lock_orders.insert({held_lock, new_lock});
          }
        }
      }
    }
  }
}

bool LockSetAnalysis::mayAlias(LockID lock1, LockID lock2) const {
  if (lock1 == lock2)
    return true;

  // Use alias analysis wrapper if available
  if (m_alias_analysis && lock1 && lock2) {
    return m_alias_analysis->mayAlias(lock1, lock2);
  }

  // Conservative: assume may alias if no analysis available
  return true;
}

LockID LockSetAnalysis::getCanonicalLock(LockID lock) const {
  // Strip pointer casts to get canonical form
  return lock->stripPointerCasts();
}

bool LockSetAnalysis::isLockOperation(const Instruction *inst) const {
  return m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst);
}

LockID LockSetAnalysis::getLockValue(const Instruction *inst) const {
  if (m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst)) {
    return getCanonicalLock(m_thread_api->getLockVal(inst));
  }
  return nullptr;
}

// ============================================================================
// Interprocedural Analysis Implementation
// ============================================================================

std::set<Function *> LockSetAnalysis::getCallees(const CallInst *call) const {
  std::set<Function *> callees;
  
  if (!call) {
    return callees;
  }
  
  // Try direct call first
  if (Function *direct_callee = call->getCalledFunction()) {
    if (!direct_callee->isDeclaration()) {
      callees.insert(direct_callee);
    }
    return callees;
  }
  
  // For indirect calls, use call graph if available
  if (m_call_graph) {
    Function *caller = const_cast<Function*>(call->getFunction());
    if (CallGraphNode *cgNode = (*m_call_graph)[caller]) {
      // Iterate over all callees from this caller
      for (auto &callRecord : *cgNode) {
        if (Function *callee = callRecord.second->getFunction()) {
          if (!callee->isDeclaration()) {
            callees.insert(callee);
          }
        }
      }
    }
  }
  
  return callees;
}

void LockSetAnalysis::computeFunctionSummary(Function *func) {
  if (!func || func->isDeclaration()) {
    return;
  }
  
  auto &summary = m_function_summaries[func];
  if (summary.is_analyzed) {
    return;
  }
  
  errs() << "Computing summary for function: " << func->getName() << "\n";
  
  // Computes a summary of lock behaviors for the function to enable interprocedural analysis.
  // - MayAcquire: Locks that *may* be acquired and held upon return.
  // - MustAcquire: Locks that *must* be acquired and held upon return.
  // - Releases: Locks released within the function.
  // This summary allows callers to update their locksets without re-analyzing the callee inline.

  // Run intraprocedural analysis to get flow-sensitive results
  computeIntraproceduralLockSets(func);
  
  summary.may_acquire.clear();
  summary.must_acquire.clear();
  summary.may_release.clear();
  summary.must_release.clear();
  
  // Collect return instructions
  std::vector<const ReturnInst *> returns;
  for (const BasicBlock &bb : *func) {
    if (const ReturnInst *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
      returns.push_back(ret);
    }
  }
  
  if (!returns.empty()) {
    // Compute must_acquire (Intersection of exit sets)
    auto it = m_must_locksets_exit.find(returns[0]);
    if (it != m_must_locksets_exit.end()) {
      summary.must_acquire = it->second;
    }
    
    for (size_t i = 1; i < returns.size(); ++i) {
      auto it = m_must_locksets_exit.find(returns[i]);
      if (it != m_must_locksets_exit.end()) {
        LockSet intersection;
        std::set_intersection(summary.must_acquire.begin(), summary.must_acquire.end(),
                              it->second.begin(), it->second.end(),
                              std::inserter(intersection, intersection.begin()));
        summary.must_acquire = intersection;
      } else {
        summary.must_acquire.clear();
      }
    }
    
    // Compute may_acquire (Union of exit sets)
    for (const auto *ret : returns) {
      auto it = m_may_locksets_exit.find(ret);
      if (it != m_may_locksets_exit.end()) {
        summary.may_acquire.insert(it->second.begin(), it->second.end());
      }
    }
  }
  
  // Handle releases
  // summary.must_release (used to update caller's must_locks) should contain MayReleased locks
  // summary.may_release (used to update caller's may_locks) should contain MustReleased locks
  
  // We scan for all releases to populate summary.must_release (MayReleased)
  for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
    Instruction *inst = &*I;
    if (m_thread_api->isTDRelease(inst)) {
      LockID lock = getLockValue(inst);
      if (lock) {
        summary.must_release.insert(lock);
      }
    }
  }
  
  // We leave summary.may_release empty (MustReleased) as we can't compute it safely
  // This is safe for may-analysis (we won't remove locks that might still be held)
  
  summary.is_analyzed = true;
  
  errs() << "  May acquire (at exit): " << summary.may_acquire.size() << " locks\n";
  errs() << "  Must acquire (at exit): " << summary.must_acquire.size() << " locks\n";
}

void LockSetAnalysis::applyFunctionSummary(const CallInst *call,
                                           const Function *callee,
                                           LockSet &may_locks,
                                           LockSet &must_locks) const {
  if (!call || !callee) {
    return;
  }
  
  auto it = m_function_summaries.find(callee);
  if (it == m_function_summaries.end() || !it->second.is_analyzed) {
    return;
  }
  
  const FunctionSummary &summary = it->second;
  
  // Apply lock acquisitions
  may_locks.insert(summary.may_acquire.begin(), summary.may_acquire.end());
  
  // For must-locks, be conservative
  // Only add to must_locks if the callee must-acquires the lock
  for (LockID lock : summary.must_acquire) {
    must_locks.insert(lock);
  }
  
  // Apply lock releases (remove from locksets)
  for (LockID lock : summary.may_release) {
    may_locks.erase(lock);
    
    // Also remove aliases if alias analysis is available
    if (m_alias_analysis) {
      LockSet to_remove;
      for (const auto *l : may_locks) {
        if (mayAlias(l, lock)) {
          to_remove.insert(l);
        }
      }
      for (const auto *l : to_remove) {
        may_locks.erase(l);
      }
    }
  }
  
  for (LockID lock : summary.must_release) {
    must_locks.erase(lock);
    
    // Also remove aliases if alias analysis is available
    if (m_alias_analysis) {
      LockSet to_remove;
      for (const auto *l : must_locks) {
        if (mayAlias(l, lock)) {
          to_remove.insert(l);
        }
      }
      for (const auto *l : to_remove) {
        must_locks.erase(l);
      }
    }
  }
}

void LockSetAnalysis::bottomUpTraversal() {
  if (!m_call_graph) {
    return;
  }
  
  errs() << "Performing bottom-up call graph traversal...\n";
  
  // Compute post-order traversal for bottom-up analysis using LLVM CallGraph
  std::vector<Function *> post_order;
  std::set<Function *> visited;
  std::stack<std::pair<Function *, bool>> stack;
  
  // Start from all functions in the module
  for (Function &func : *m_module) {
    if (!func.isDeclaration()) {
      if (visited.find(&func) == visited.end()) {
        stack.push({&func, false});
        
        while (!stack.empty()) {
          auto top_pair = stack.top();
          stack.pop();
          Function *current = top_pair.first;
          bool children_visited = top_pair.second;
          
          if (children_visited) {
            post_order.push_back(current);
            continue;
          }
          
          if (visited.find(current) != visited.end()) {
            continue;
          }
          
          visited.insert(current);
          stack.push({current, true});
          
          // Get callees from LLVM CallGraph
          if (CallGraphNode *cgNode = (*m_call_graph)[current]) {
            for (auto &callRecord : *cgNode) {
              if (Function *callee = callRecord.second->getFunction()) {
                if (callee && !callee->isDeclaration() && 
                    visited.find(callee) == visited.end()) {
                  stack.push({callee, false});
                }
              }
            }
          }
        }
      }
    }
  }
  
  errs() << "Processing " << post_order.size() << " functions in bottom-up order\n";
  
  // Compute summaries in post-order (callees before callers)
  for (Function *func : post_order) {
    computeFunctionSummary(func);
  }
  
  errs() << "Bottom-up traversal complete\n";
}
