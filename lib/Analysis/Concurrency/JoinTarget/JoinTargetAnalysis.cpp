/**
 * @file JoinTargetAnalysis.cpp
 * @brief Join-target set implementation
 */

#include "Analysis/Concurrency/JoinTarget/JoinTargetAnalysis.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace lotus;

namespace mhp {

JoinTargetAnalysis::JoinTargetAnalysis(Module &module,
                                       AliasAnalysisWrapper *aliasAnalysis)
    : m_module(module), m_threadAPI(ThreadAPI::getThreadAPI()),
      m_aliasAnalysis(aliasAnalysis) {}

void JoinTargetAnalysis::analyze() {
  collectForksAndJoins();
  m_joinToForks.clear();

  auto mayAlias = [this](const Value *a, const Value *b) {
    if (!a || !b) return false;
    if (a->stripPointerCasts() == b->stripPointerCasts()) return true;
    if (m_aliasAnalysis) return m_aliasAnalysis->mayAlias(a, b);
    return true;
  };

  for (const Instruction *joinInst : m_joinInsts) {
    const CallBase *joinCall = dyn_cast<CallBase>(joinInst);
    if (!joinCall || joinCall->arg_size() < 1) continue;
    const Value *joinArg0 = joinCall->getArgOperand(0);

    std::vector<const Instruction *> forks;
    for (const Instruction *forkInst : m_forkInsts) {
      const CallBase *forkCall = dyn_cast<CallBase>(forkInst);
      if (!forkCall || forkCall->arg_size() < 1) continue;
      const Value *forkArg0 = forkCall->getArgOperand(0);
      if (mayAlias(joinArg0, forkArg0))
        forks.push_back(forkInst);
    }
    m_joinToForks[joinInst] = std::move(forks);
  }
}

void JoinTargetAnalysis::collectForksAndJoins() {
  m_forkInsts.clear();
  m_joinInsts.clear();
  for (Function &F : m_module) {
    if (F.isDeclaration()) continue;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;
      if (m_threadAPI->isTDFork(inst))
        m_forkInsts.push_back(inst);
      else if (m_threadAPI->isTDJoin(inst))
        m_joinInsts.push_back(inst);
    }
  }
}

std::vector<const Instruction *>
JoinTargetAnalysis::getPossibleJoinedForks(const Instruction *joinInst) const {
  auto it = m_joinToForks.find(joinInst);
  if (it != m_joinToForks.end())
    return it->second;
  return {};
}

bool JoinTargetAnalysis::isUnambiguousJoin(const Instruction *joinInst) const {
  return getPossibleJoinedForks(joinInst).size() == 1;
}

} // namespace mhp
