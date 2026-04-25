/**
 * @file JoinTargetAnalysis.cpp
 * @brief Join-target analysis facade and orchestration
 */

#include "Concurrency/JoinTarget/JoinTargetAnalysis.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/IR/InstIterator.h>

using namespace llvm;
using namespace lotus;

namespace mhp {

JoinTargetAnalysis::JoinTargetAnalysis(Module &module,
                                       AliasAnalysisWrapper *aliasAnalysis)
    : m_module(module), m_threadAPI(ThreadAPI::getThreadAPI()),
      m_aliasAnalysis(aliasAnalysis) {}

void JoinTargetAnalysis::analyze() {
  collectForksAndJoins();
  m_forkToRoot.clear();
  m_joinToForks.clear();
  m_joinToFeasibleForks.clear();
  m_joinHasUnknownLiveFork.clear();
  m_unambiguousJoins.clear();
  m_functionSummaries.clear();
  m_blockInStates.clear();
  m_blockOutStates.clear();

  if (!m_threadMultiplicity) {
    m_threadMultiplicity =
        std::make_unique<concurrency::ThreadMultiplicityAnalysis>(m_module);
  }

  for (const Instruction *forkInst : m_forkInsts) {
    m_forkToRoot[forkInst] = traceThreadHandleRoot(
        m_threadAPI->getForkedThread(forkInst), &m_module);
  }

  buildFunctionSummaries();

  for (const Function &func : m_module) {
    if (!func.isDeclaration()) {
      analyzeFunction(func);
    }
  }

  for (const Instruction *joinInst : m_joinInsts) {
    auto feasibleIt = m_joinToFeasibleForks.find(joinInst);
    if (feasibleIt == m_joinToFeasibleForks.end() ||
        feasibleIt->second.size() != 1 ||
        m_joinHasUnknownLiveFork[joinInst]) {
      continue;
    }

    const Instruction *targetFork = feasibleIt->second.front();
    if (!m_threadMultiplicity->instructionMayExecuteMultipleTimes(targetFork)) {
      m_unambiguousJoins.insert(joinInst);
    }
  }
}

void JoinTargetAnalysis::collectForksAndJoins() {
  m_forkInsts.clear();
  m_joinInsts.clear();
  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;
      if (m_threadAPI->isTDFork(inst)) {
        m_forkInsts.push_back(inst);
      } else if (m_threadAPI->isTDJoin(inst)) {
        m_joinInsts.push_back(inst);
      }
    }
  }
}

std::vector<const Instruction *>
JoinTargetAnalysis::getPossibleJoinedForks(const Instruction *joinInst) const {
  auto it = m_joinToForks.find(joinInst);
  if (it != m_joinToForks.end()) {
    return it->second;
  }
  return {};
}

std::vector<const Instruction *>
JoinTargetAnalysis::getFeasibleJoinedForks(const Instruction *joinInst) const {
  auto it = m_joinToFeasibleForks.find(joinInst);
  if (it != m_joinToFeasibleForks.end()) {
    return it->second;
  }
  return {};
}

const Instruction *JoinTargetAnalysis::getDefiniteFeasibleJoinedFork(
    const Instruction *joinInst) const {
  if (!isUnambiguousJoin(joinInst)) {
    return nullptr;
  }
  auto feasible = getFeasibleJoinedForks(joinInst);
  if (feasible.size() != 1) {
    return nullptr;
  }
  return feasible.front();
}

bool JoinTargetAnalysis::isUnambiguousJoin(const Instruction *joinInst) const {
  return m_unambiguousJoins.count(joinInst) != 0;
}

JoinTargetAnalysis::CandidateCountKind JoinTargetAnalysis::classifyJoinForks(
    const std::vector<const Instruction *> &forks) const {
  if (forks.empty()) {
    return CandidateCountKind::Zero;
  }
  if (forks.size() == 1) {
    return CandidateCountKind::One;
  }
  return CandidateCountKind::Many;
}

} // namespace mhp
