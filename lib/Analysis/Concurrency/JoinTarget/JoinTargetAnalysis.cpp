/**
 * @file JoinTargetAnalysis.cpp
 * @brief Join-target set implementation
 */

#include "Analysis/Concurrency/JoinTarget/JoinTargetAnalysis.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

#include <deque>
#include <set>

using namespace llvm;
using namespace lotus;

namespace mhp {

const Value *JoinTargetAnalysis::traceThreadHandleRoot(const Value *value,
                                                       const Module *module) {
  if (!value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolved_root = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    const Value *stripped = current->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped)) {
      if (!resolved_root) {
        resolved_root = stripped;
      } else if (resolved_root != stripped) {
        return nullptr;
      }
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(stripped)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }

    if (const auto *store = dyn_cast<StoreInst>(stripped)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
      continue;
    }

    if (const auto *gep = dyn_cast<GetElementPtrInst>(stripped)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (const auto *phi = dyn_cast<PHINode>(stripped)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }

    if (const auto *select = dyn_cast<SelectInst>(stripped)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(stripped)) {
      if (!module) {
        return stripped;
      }
      const Function *parent = arg->getParent();
      bool expanded = false;
      if (parent) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (!cb || arg->getArgNo() >= cb->arg_size()) {
            continue;
          }
          worklist.push_back(cb->getArgOperand(arg->getArgNo()));
          expanded = true;
        }

        for (const Function &func : *module) {
          for (const BasicBlock &bb : func) {
            for (const Instruction &inst : bb) {
              const auto *cb = dyn_cast<CallBase>(&inst);
              if (!cb || arg->getArgNo() >= cb->arg_size()) {
                continue;
              }
              const Value *called = cb->getCalledOperand();
              if (called && called->stripPointerCasts() == parent) {
                worklist.push_back(cb->getArgOperand(arg->getArgNo()));
                expanded = true;
              }
            }
          }
        }
      }
      if (!expanded) {
        if (!resolved_root) {
          resolved_root = stripped;
        } else if (resolved_root != stripped) {
          return nullptr;
        }
      }
      continue;
    }

    if (const auto *inst = dyn_cast<Instruction>(stripped)) {
      for (const Use &operand : inst->operands()) {
        worklist.push_back(operand.get());
      }
      continue;
    }

    if (!resolved_root) {
      resolved_root = stripped;
    } else if (resolved_root != stripped) {
      return nullptr;
    }
  }

  return resolved_root;
}

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
    const Value *joinArg0 =
        traceThreadHandleRoot(m_threadAPI->getJoinedThread(joinInst), &m_module);

    std::vector<const Instruction *> forks;
    for (const Instruction *forkInst : m_forkInsts) {
      const Value *forkArg0 =
          traceThreadHandleRoot(m_threadAPI->getForkedThread(forkInst), &m_module);
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
