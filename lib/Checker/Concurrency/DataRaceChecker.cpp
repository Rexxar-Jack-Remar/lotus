/*
 *
 * Author: rainoftime
*/
#include "Checker/Concurrency/DataRaceChecker.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/HappensBeforeAnalysis.h"
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

#include <unordered_map>
#include <unordered_set>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

DataRaceChecker::DataRaceChecker(Module& module, MHPAnalysis* mhpAnalysis,
                                 LockSetAnalysis* locksetAnalysis,
                                 EscapeAnalysis* escapeAnalysis,
                                 AliasAnalysisWrapper* aliasAnalysis,
                                 HappensBeforeAnalysis* happensBeforeAnalysis)
    : m_module(module), m_mhpAnalysis(mhpAnalysis),
      m_locksetAnalysis(locksetAnalysis), m_escapeAnalysis(escapeAnalysis),
      m_aliasAnalysis(aliasAnalysis), m_happensBeforeAnalysis(happensBeforeAnalysis),
      m_threadAPI(ThreadAPI::getThreadAPI()) {}

bool DataRaceChecker::areIndependent(const Instruction* inst1,
                                     const Instruction* inst2) const {
  return !mayAccessSameLocation(inst1, inst2);
}

// Types that are never considered racy (sync objects, FILE, etc.). Borrowed from Goblint.
static bool isIgnorableTypeForRace(const Type* ty) {
  if (!ty || !ty->isPointerTy()) return false;
  const Type* elem = cast<PointerType>(ty)->getPointerElementType();
  const StructType* st = dyn_cast<StructType>(elem);
  if (!st || !st->hasName()) return false;
  StringRef name = st->getName();
  // Strip LLVM name prefix if present
  if (name.startswith("\01")) name = name.drop_front(1);
  static const char* ignorable[] = {
      "pthread_mutex_t", "pthread_cond_t", "pthread_barrier_t", "pthread_rwlock_t",
      "pthread_spinlock_t", "pthread_once_t", "__pthread_mutex_s",
      "__pthread_cond_s", "__pthread_rwlock_arch_t", "FILE", "__FILE", "_IO_FILE",
      "atomic_flag", "atomic_t", "spinlock_t", "pthread_condattr_t",
      "pthread_mutexattr_t", "pthread_barrierattr_t", "__jmp_buf_tag",
      "_pthread_cleanup_buffer", "__cancel_jmp_buf_tag", "lock_class_key",
  };
  for (const char* ig : ignorable)
    if (name.equals(ig)) return true;
  if (name.startswith("__anon")) return true;  // anonymous sync structs
  return false;
}

// Single predicate for "would we report a data race for this pair?" (borrowed from Goblint MCP idea).
bool DataRaceChecker::wouldReportDataRace(const Instruction* inst1,
                                          const Instruction* inst2) const {
  if (isAtomicOperation(inst1) || isAtomicOperation(inst2)) return false;
  if (!isWriteAccess(inst1) && !isWriteAccess(inst2)) return false;
  if (!m_mhpAnalysis->mayHappenInParallel(inst1, inst2)) return false;
  if (m_happensBeforeAnalysis &&
      (m_happensBeforeAnalysis->happensBefore(inst1, inst2) ||
       m_happensBeforeAnalysis->happensBefore(inst2, inst1)))
    return false;
  if (areIndependent(inst1, inst2)) return false;

  // Suppress only with MUST mutual exclusion (sound for recall).
  if (m_locksetAnalysis) {
    const bool w1 = isWriteAccess(inst1);
    const bool w2 = isWriteAccess(inst2);

    if (w1 || w2) {
      // A write must be protected by a write-lock; a read can be protected by
      // either a mutex/write-lock or an rwlock read-lock.
      const auto mustProtectedSet = [this](const Instruction *I, bool isWrite) {
        return isWrite ? m_locksetAnalysis->getMustWriteLockSetAt(I)
                       : m_locksetAnalysis->getMustLockSetAt(I);
      };

      const LockSet s1 = mustProtectedSet(inst1, w1);
      const LockSet s2 = mustProtectedSet(inst2, w2);
      for (const auto *l : s1) {
        if (s2.find(l) != s2.end())
          return false;
      }
    }
  }
  return true;
}

// Detects data races by checking all pairs of memory accesses.
// A data race occurs when:
//   1. Two instructions may happen in parallel (MHP analysis)
//   2. At least one is a write operation
//   3. They may access the same memory location (alias analysis)
//   4. Neither operation is atomic
//   5. They are not protected by a common lock (LockSet analysis)
//   6. The memory location is shared/escaped (Escape analysis)
// Reports one bug per "racy component" (connected set of conflicting accesses), not per pair (borrowed from Goblint).
std::vector<ConcurrencyBugReport> DataRaceChecker::checkDataRaces() {
    buildSyncObjectSet();
    std::vector<const Instruction*> accesses;
    collectVariableAccesses(accesses);

    using Pair = std::pair<const Instruction*, const Instruction*>;
    std::vector<Pair> racyPairs;

    // Group accesses by underlying base object to avoid quadratic scans across unrelated objects.
    std::unordered_map<const Value *, std::vector<const Instruction *>> byBase;
    byBase.reserve(accesses.size());
    for (const Instruction *I : accesses) {
      const Value *loc = getMemoryLocation(I);
      if (!loc) continue;
      const Value *base = llvm::getUnderlyingObject(loc);
      byBase[base].push_back(I);
    }

    // Collect racy pairs using single predicate (within each base object group).
    for (auto &kv : byBase) {
      auto &vec = kv.second;
      for (size_t i = 0; i < vec.size(); ++i) {
        const Instruction *inst1 = vec[i];
        for (size_t j = i + 1; j < vec.size(); ++j) {
          const Instruction *inst2 = vec[j];
          if (wouldReportDataRace(inst1, inst2))
            racyPairs.emplace_back(inst1, inst2);
        }
      }
    }

    // Build union-find over instructions that appear in any racy pair (one report per component)
    std::unordered_map<const Instruction*, const Instruction*> parent;
    auto findRoot = [&parent](const Instruction* i) -> const Instruction* {
        const Instruction* cur = i;
        std::vector<const Instruction*> path;
        for (;;) {
            auto it = parent.find(cur);
            if (it == parent.end()) return cur;
            if (it->second == cur) {  // root
                for (const Instruction* p : path) parent[p] = cur;
                return cur;
            }
            path.push_back(cur);
            cur = it->second;
        }
    };
    auto unite = [&findRoot, &parent](const Instruction* a, const Instruction* b) {
        const Instruction* ra = findRoot(a);
        const Instruction* rb = findRoot(b);
        if (ra != rb) parent[ra] = rb;
    };
    for (const Pair& p : racyPairs) {
        if (parent.find(p.first) == parent.end()) parent[p.first] = p.first;
        if (parent.find(p.second) == parent.end()) parent[p.second] = p.second;
        unite(p.first, p.second);
    }

    // Group by root: root -> list of instructions in component
    std::unordered_map<const Instruction*, std::vector<const Instruction*>> components;
    for (auto& kv : parent) {
        const Instruction* root = findRoot(kv.first);
        components[root].push_back(kv.first);
    }

    // One report per component (representative pair = first two in component)
    std::vector<ConcurrencyBugReport> reports;
    for (auto& kv : components) {
        std::vector<const Instruction*>& comp = kv.second;
        if (comp.empty()) continue;
        const Instruction* rep1 = comp[0];
        const Instruction* rep2 = comp.size() > 1 ? comp[1] : comp[0];
        std::string desc = "Potential data race between " + getInstructionLocation(rep1) +
                          " and " + getInstructionLocation(rep2);
        if (comp.size() > 2)
            desc += " (" + std::to_string(comp.size()) + " conflicting accesses)";

        ConcurrencyBugReport report(ConcurrencyBugType::DATA_RACE, desc,
                                    BugDescription::BI_HIGH, BugDescription::BC_ERROR);
        report.setDataRaceInfo(getAccessPath(rep1), getAccessPath(rep2),
                              isWriteAccess(rep1), isWriteAccess(rep2),
                              getAccessPath(rep1) + " / " + getAccessPath(rep2));
        for (const Instruction* inst : comp)
            report.addStep(inst, isWriteAccess(inst) ? "Write" : "Read");
        reports.push_back(std::move(report));
    }
    return reports;
}

// Collect all candidate memory accesses into a flat list so that alias
// checks catch distinct pointer expressions referencing the same memory.
void DataRaceChecker::collectVariableAccesses(
    std::vector<const Instruction*>& accesses) {
    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;
        for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
            if (isMemoryAccess(&*I)) {
                const Value* memLoc = getMemoryLocation(&*I);
                    if (memLoc) {
                    if (isSyncObjectAccess(memLoc)) continue;
                    if (isIgnorableTypeForRace(memLoc->getType())) continue;
                    if (m_escapeAnalysis && !m_escapeAnalysis->isEscaped(memLoc)) {
                        // If it's a local variable that hasn't escaped, it can't race
                        // Check if it's a stack allocation (AllocaInst)
                        const Value* baseObj = memLoc->stripPointerCasts();
                        if (isa<AllocaInst>(baseObj)) {
                             continue;
                        }
                        // For other types (globals, etc), let escape analysis decide.
                        // Do NOT discard the access here so that shared globals are considered.
                    }
                    accesses.push_back(&*I);
                }
            }
        }
    }
}

// Checks if two instructions may access the same memory location using alias analysis.
bool DataRaceChecker::mayAccessSameLocation(const Instruction* inst1,
                                            const Instruction* inst2) const {
    return mayAlias(getMemoryLocation(inst1), getMemoryLocation(inst2));
}

// Returns true if two values may alias (point to overlapping memory).
// Uses alias analysis wrapper when available, otherwise conservatively assumes aliasing.
bool DataRaceChecker::mayAlias(const Value* v1, const Value* v2) const {
    if (!v1 || !v2) return false;
    if (v1 == v2) return true;
    if (m_aliasAnalysis) {
        return m_aliasAnalysis->mayAlias(v1, v2);
    }
    return true;  // Conservative: assume may alias if we can't prove otherwise.
}

bool DataRaceChecker::isMemoryAccess(const Instruction* inst) const {
    return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
           isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool DataRaceChecker::isWriteAccess(const Instruction* inst) const {
    return isa<StoreInst>(inst) || isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool DataRaceChecker::isAtomicOperation(const Instruction* inst) const {
    if (isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst)) return true;
    if (const auto *L = dyn_cast<LoadInst>(inst)) return L->isAtomic();
    if (const auto *S = dyn_cast<StoreInst>(inst)) return S->isAtomic();
    return false;
}

// Extracts the memory location (pointer operand) from a memory access instruction.
const Value* DataRaceChecker::getMemoryLocation(const Instruction* inst) const {
    if (const auto* load = dyn_cast<LoadInst>(inst))
        return load->getPointerOperand();
    if (const auto* store = dyn_cast<StoreInst>(inst))
        return store->getPointerOperand();
    if (const auto* rmw = dyn_cast<AtomicRMWInst>(inst))
        return rmw->getPointerOperand();
    if (const auto* cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst))
        return cmpxchg->getPointerOperand();
    return nullptr;
}

std::string DataRaceChecker::getInstructionLocation(const Instruction* inst) const {
    std::string location;
    raw_string_ostream os(location);
    if (const Function* func = inst->getFunction())
        os << func->getName();
    if (const BasicBlock* bb = inst->getParent())
        os << ":" << bb->getName();
    return os.str();
}

void DataRaceChecker::buildSyncObjectSet() {
    m_syncObjects.clear();
    for (Function& F : m_module) {
        if (F.isDeclaration()) continue;
        for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
            const Instruction* inst = &*I;
            const CallBase* cb = dyn_cast<CallBase>(inst);
            if (!cb || !m_threadAPI->getCallee(inst)) continue;
            const Value* v = nullptr;
            if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst))
                v = m_threadAPI->getLockVal(inst);
            else if (m_threadAPI->isTDCondWait(inst) || m_threadAPI->isTDCondSignal(inst) ||
                     m_threadAPI->isTDCondBroadcast(inst))
                v = m_threadAPI->getCondVal(inst);
            else if (m_threadAPI->isTDBarWait(inst))
                v = m_threadAPI->getBarrierVal(inst);
            if (v) m_syncObjects.insert(v->stripPointerCasts());
        }
    }
}

bool DataRaceChecker::isSyncObjectAccess(const Value* loc) const {
    if (!loc) return false;
    Value* stripped = const_cast<Value*>(loc)->stripPointerCasts();
    if (m_syncObjects.count(stripped)) return true;
    if (!m_aliasAnalysis) return false;
    for (const Value* sync : m_syncObjects)
        if (m_aliasAnalysis->mayAlias(stripped, sync)) return true;
    return false;
}

std::string DataRaceChecker::getAccessPath(const Instruction* inst) const {
    const Value* loc = getMemoryLocation(inst);
    if (!loc) return getInstructionLocation(inst);
    std::string s;
    raw_string_ostream os(s);
    loc->printAsOperand(os, true);
    return os.str();
}

} // namespace concurrency
