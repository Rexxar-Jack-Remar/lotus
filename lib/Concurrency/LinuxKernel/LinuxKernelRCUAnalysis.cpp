/**
 * @file LinuxKernelRCUAnalysis.cpp
 * @brief Linux Kernel RCU Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Concurrency/LinuxKernel/LinuxKernelRCUAnalysis.h"

#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>

using namespace llvm;

namespace kernel {

void LinuxKernelRCUAnalysis::analyzeRCU() {
  read_sections_.clear();
  grace_periods_.clear();
  rcu_diagnostics_.clear();

  identifyReadSections();
  identifyGracePeriods();
  matchCallbacksToGracePeriods();

  rcu_diagnostics_["total_read_sections"] = read_sections_.size();
  rcu_diagnostics_["total_grace_periods"] = grace_periods_.size();
}

void LinuxKernelRCUAnalysis::identifyReadSections() {
  struct OpenSection {
    const KernelOperation *outer = nullptr;
    unsigned depth = 0;
  };
  std::map<std::tuple<const Function *, RCUFlavor, const Value *>, OpenSection>
      open_sections;

  for (const auto &op : process_model_.getAllOperations()) {
    auto key =
        std::make_tuple(op.inst->getFunction(), op.rcu_flavor, op.rcu_domain);
    auto &open = open_sections[key];
    if (op.kind == OperationKind::RCU_READ_LOCK) {
      if (open.depth++ == 0) {
        open.outer = &op;
      }
      continue;
    }

    if (op.kind != OperationKind::RCU_READ_UNLOCK || open.depth == 0) {
      continue;
    }
    if (--open.depth != 0) {
      continue;
    }

    RCUCriticalSection section;
    section.read_lock = open.outer->inst;
    section.read_unlock = op.inst;
    section.function = op.inst->getFunction();
    section.domain = op.rcu_domain;
    section.flavor = op.rcu_flavor;

    DominatorTree dominators(*const_cast<Function *>(section.function));
    PostDominatorTree post_dominators;
    post_dominators.recalculate(*const_cast<Function *>(section.function));

    for (const KernelOperation *candidate :
         process_model_.getOperationsInFunction(section.function)) {
      if (!dominators.dominates(section.read_lock, candidate->inst) ||
          !post_dominators.dominates(section.read_unlock, candidate->inst)) {
        continue;
      }
      if (candidate->kind == OperationKind::RCU_DEREFERENCE ||
          candidate->kind == OperationKind::RCU_ASSIGN ||
          candidate->kind == OperationKind::ATOMIC_READ ||
          candidate->kind == OperationKind::ATOMIC_RMW) {
        section.protected_accesses.push_back(candidate->inst);
      }
    }

    read_sections_.push_back(section);
    open.outer = nullptr;
  }
}

void LinuxKernelRCUAnalysis::identifyGracePeriods() {
  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind != OperationKind::RCU_SYNC &&
        op.kind != OperationKind::RCU_CALL) {
      continue;
    }

    RCUGracePeriod grace_period;
    grace_period.sync_inst = op.inst;
    grace_period.function = op.inst->getFunction();
    grace_period.domain = op.rcu_domain;
    grace_period.flavor = op.rcu_flavor;
    if (op.kind == OperationKind::RCU_CALL) {
      grace_period.callbacks.push_back(op.inst);
    }
    grace_periods_.push_back(grace_period);
  }
}

void LinuxKernelRCUAnalysis::matchCallbacksToGracePeriods() {
  for (auto &section : read_sections_) {
    for (const auto &grace_period : grace_periods_) {
      const KernelOperation *grace_op =
          process_model_.getOperationForInstruction(grace_period.sync_inst);
      if (grace_op == nullptr || grace_op->kind != OperationKind::RCU_SYNC) {
        continue;
      }
      if (grace_period.function != section.function) {
        continue;
      }
      if (grace_period.flavor != section.flavor ||
          grace_period.domain != section.domain) {
        continue;
      }
      DominatorTree dominators(*const_cast<Function *>(section.function));
      if (!dominators.dominates(section.read_unlock, grace_period.sync_inst)) {
        continue;
      }
      section.has_sync = true;
      section.sync_point = grace_period.sync_inst;
      break;
    }
  }
}

bool LinuxKernelRCUAnalysis::isWithinRCUSection(const Instruction *inst) const {
  return getEnclosingSection(inst) != nullptr;
}

const LinuxKernelRCUAnalysis::RCUCriticalSection *
LinuxKernelRCUAnalysis::getEnclosingSection(const Instruction *inst) const {
  if (inst == nullptr) {
    return nullptr;
  }

  for (const auto &section : read_sections_) {
    if (section.function != inst->getFunction()) {
      continue;
    }
    DominatorTree dominators(*const_cast<Function *>(section.function));
    PostDominatorTree post_dominators;
    post_dominators.recalculate(*const_cast<Function *>(section.function));
    if (!dominators.dominates(section.read_lock, inst) ||
        (section.read_unlock != nullptr &&
         !post_dominators.dominates(section.read_unlock, inst))) {
      continue;
    }
    return &section;
  }

  return nullptr;
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findReadSideWithoutGracePeriod() const {
  // Readers do not owe a grace period.  Reclamation is an updater-side
  // obligation after unpublishing an object.
  return {};
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelRCUAnalysis::findRCUConflicts() const {
  // Reader/updater overlap is a normal property of RCU.  A conflict requires
  // an explicit unsafe reclamation relation, not merely an assignment during
  // a lexically scanned reader section.
  return {};
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findRCUDoubleFree() const {
  // Multiple callback submissions involving the same domain are not a double
  // free.  Proving duplicate reclamation requires callback/lifetime state.
  return {};
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findDerefAfterFree() const {
  // synchronize_rcu() waits for pre-existing readers; it does not free an
  // object.  Do not invent a release point from a grace-period call.
  return {};
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findUnsafeReclamation() const {
  std::vector<const Instruction *> result;
  std::map<const Function *, std::unique_ptr<DominatorTree>> dominators;

  for (const auto &reclaim : process_model_.getAllOperations()) {
    if (reclaim.kind != OperationKind::RCU_RECLAIM ||
        reclaim.rcu_target == nullptr) {
      continue;
    }

    const auto *unpublish_inst = dyn_cast<Instruction>(reclaim.rcu_target);
    const KernelOperation *unpublish =
        process_model_.getOperationForInstruction(unpublish_inst);
    if (unpublish == nullptr || unpublish->kind != OperationKind::RCU_ASSIGN ||
        !unpublish->returns_retired_pointer) {
      continue;
    }

    const Function *function = reclaim.inst->getFunction();
    auto &dt = dominators[function];
    if (!dt) {
      dt = std::make_unique<DominatorTree>(*const_cast<Function *>(function));
    }

    bool safe = false;
    for (const auto &sync : process_model_.getAllOperations()) {
      if (sync.kind != OperationKind::RCU_SYNC ||
          sync.inst->getFunction() != function ||
          sync.rcu_flavor != unpublish->rcu_flavor ||
          sync.rcu_domain != unpublish->rcu_domain) {
        continue;
      }
      if (dt->dominates(unpublish->inst, sync.inst) &&
          dt->dominates(sync.inst, reclaim.inst)) {
        safe = true;
        break;
      }
    }
    if (!safe) {
      result.push_back(reclaim.inst);
    }
  }

  return result;
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findDerefInWrongSection() const {
  std::vector<const Instruction *> result;
  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind == OperationKind::RCU_DEREFERENCE && op.requires_rcu_section &&
        !isWithinRCUSection(op.inst)) {
      result.push_back(op.inst);
    }
  }
  return result;
}

} // namespace kernel
