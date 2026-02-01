/*
 *
 * Author: rainoftime
*/
#include "Checker/Concurrency/DataRaceChecker.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/HappensBeforeAnalysis.h"
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

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

// Detects data races by checking all pairs of memory accesses.
// A data race occurs when:
//   1. Two instructions may happen in parallel (MHP analysis)
//   2. At least one is a write operation
//   3. They may access the same memory location (alias analysis)
//   4. Neither operation is atomic
//   5. They are not protected by a common lock (LockSet analysis)
//   6. The memory location is shared/escaped (Escape analysis)
std::vector<ConcurrencyBugReport> DataRaceChecker::checkDataRaces() {
    buildSyncObjectSet();
    std::vector<ConcurrencyBugReport> reports;
    std::vector<const Instruction*> accesses;
    collectVariableAccesses(accesses);

    // Compare all accesses pairwise with alias + MHP filters to avoid
    // missing distinct pointers that still alias.
    for (size_t i = 0; i < accesses.size(); ++i) {
        const Instruction* inst1 = accesses[i];
        if (isAtomicOperation(inst1)) continue;  // Atomic operations prevent races.

        for (size_t j = i + 1; j < accesses.size(); ++j) {
            const Instruction* inst2 = accesses[j];
            if (isAtomicOperation(inst2)) continue;

            // 1. At least one is a write
            if (!isWriteAccess(inst1) && !isWriteAccess(inst2)) continue;

            // 2. May run in parallel
            if (!m_mhpAnalysis->mayHappenInParallel(inst1, inst2)) continue;

            // 2b. Ordered by happens-before (e.g. C11 synchronizes-with)?
            if (m_happensBeforeAnalysis &&
                (m_happensBeforeAnalysis->happensBefore(inst1, inst2) ||
                 m_happensBeforeAnalysis->happensBefore(inst2, inst1)))
              continue;

            // 2c. Independent (provably different locations)?
            if (areIndependent(inst1, inst2)) continue;

            // 3. Protected by common lock?
            if (m_locksetAnalysis && m_locksetAnalysis->mayHoldCommonLock(inst1, inst2)) continue;

            // 4. May access same location (alias); independence already skipped above
            if (!mayAccessSameLocation(inst1, inst2)) continue;

            ConcurrencyBugReport report(
                ConcurrencyBugType::DATA_RACE,
                "Potential data race between " + getInstructionLocation(inst1) +
                " and " + getInstructionLocation(inst2),
                BugDescription::BI_HIGH, BugDescription::BC_ERROR);

            report.setDataRaceInfo(getAccessPath(inst1), getAccessPath(inst2),
                                  isWriteAccess(inst1), isWriteAccess(inst2),
                                  getAccessPath(inst1) + " / " + getAccessPath(inst2));
            report.addStep(inst1, isWriteAccess(inst1) ? "Write" : "Read");
            report.addStep(inst2, isWriteAccess(inst2) ? "Write" : "Read");

            reports.push_back(std::move(report));
        }
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
    return isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
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
