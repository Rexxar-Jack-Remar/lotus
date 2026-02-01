/*
 *
 * Author: rainoftime
*/
#include "Analysis/Concurrency/HappensBeforeAnalysis.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

HappensBeforeAnalysis::HappensBeforeAnalysis(Module &module, mhp::MHPAnalysis &mhp)
    : m_module(module), m_mhp(mhp) {}

void HappensBeforeAnalysis::analyze() {
  buildSynchronizesWith();
}

void HappensBeforeAnalysis::buildSynchronizesWith() {
  m_sync_with.clear();
  using namespace Cpp11Atomics;

  auto isReleaseStore = [](const Instruction *inst) {
    if (!isAtomic(inst)) return false;
    MemoryOrder mo = getMemoryOrder(inst);
    return mo == MemoryOrder::Release || mo == MemoryOrder::AcquireRelease ||
           mo == MemoryOrder::SequentiallyConsistent;
  };
  auto isAcquireLoad = [](const Instruction *inst) {
    if (!isAtomic(inst)) return false;
    MemoryOrder mo = getMemoryOrder(inst);
    return mo == MemoryOrder::Acquire || mo == MemoryOrder::AcquireRelease ||
           mo == MemoryOrder::SequentiallyConsistent;
  };

  std::vector<const Instruction *> release_stores;
  std::vector<const Instruction *> acquire_loads;

  for (Function &F : m_module) {
    if (F.isDeclaration()) continue;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;
      if (isStore(inst) && isReleaseStore(inst))
        release_stores.push_back(inst);
      if (isLoad(inst) && isAcquireLoad(inst))
        acquire_loads.push_back(inst);
    }
  }

  for (const Instruction *S : release_stores) {
    for (const Instruction *L : acquire_loads) {
      if (S == L) continue;
      if (sameAtomicLocation(S, L))
        m_sync_with.emplace_back(S, L);
    }
  }
}

bool HappensBeforeAnalysis::sameAtomicLocation(const Instruction *store_inst,
                                                const Instruction *load_inst) const {
  const Value *p1 = Cpp11Atomics::getAtomicPointer(store_inst);
  const Value *p2 = Cpp11Atomics::getAtomicPointer(load_inst);
  if (!p1 || !p2) return false;
  if (p1->stripPointerCasts() == p2->stripPointerCasts()) return true;
  if (m_alias_analysis && m_alias_analysis->mayAlias(p1, p2)) return true;
  return false;
}

bool HappensBeforeAnalysis::happensBefore(const Instruction *A, const Instruction *B) const {
  if (!A || !B) return false;
  if (A == B) return true;

  auto key = std::make_pair(A, B);
  if (m_hb_cache.count(key))
    return m_hb_cache[key];

  bool result = m_mhp.mustPrecede(A, B);

  if (!result) {
    for (const auto &p : m_sync_with) {
      const Instruction *S = p.first;
      const Instruction *L = p.second;
      if (m_mhp.mustPrecede(A, S) && m_mhp.mustPrecede(L, B)) {
        result = true;
        break;
      }
    }
  }

  m_hb_cache[key] = result;
  return result;
}

} // namespace lotus

