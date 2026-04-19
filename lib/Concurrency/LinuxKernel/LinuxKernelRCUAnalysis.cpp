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
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

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
  std::map<const Function *, std::vector<const KernelOperation *>> open_sections;

  for (const auto &op : process_model_.getAllOperations()) {
    auto &stack = open_sections[op.inst->getFunction()];
    if (op.kind == OperationKind::RCU_READ_LOCK) {
      stack.push_back(&op);
      continue;
    }

    if (op.kind != OperationKind::RCU_READ_UNLOCK || stack.empty()) {
      continue;
    }

    const KernelOperation *read_lock = stack.back();
    stack.pop_back();

    RCUCriticalSection section;
    section.read_lock = read_lock->inst;
    section.read_unlock = op.inst;
    section.function = op.inst->getFunction();

    for (const KernelOperation *candidate :
         process_model_.getOperationsInFunction(section.function)) {
      if (!process_model_.isBeforeInFunction(section.read_lock, candidate->inst) ||
          !process_model_.isBeforeInFunction(candidate->inst, section.read_unlock)) {
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
  }
}

void LinuxKernelRCUAnalysis::identifyGracePeriods() {
  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind != OperationKind::RCU_SYNC) {
      continue;
    }

    RCUGracePeriod grace_period;
    grace_period.sync_inst = op.inst;
    grace_period.function = op.inst->getFunction();
    grace_periods_.push_back(grace_period);
  }
}

void LinuxKernelRCUAnalysis::matchCallbacksToGracePeriods() {
  for (auto &section : read_sections_) {
    for (const auto &grace_period : grace_periods_) {
      if (grace_period.function != section.function) {
        continue;
      }
      if (!process_model_.isBeforeInFunction(section.read_unlock,
                                             grace_period.sync_inst)) {
        continue;
      }
      section.has_sync = true;
      section.sync_point = grace_period.sync_inst;
      break;
    }
  }

  for (auto &grace_period : grace_periods_) {
    for (const auto &op : process_model_.getAllOperations()) {
      if (op.kind != OperationKind::RCU_CALL ||
          op.inst->getFunction() != grace_period.function) {
        continue;
      }
      if (process_model_.isBeforeInFunction(grace_period.sync_inst, op.inst)) {
        grace_period.callbacks.push_back(op.inst);
      }
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
    if (!process_model_.isBeforeInFunction(section.read_lock, inst)) {
      continue;
    }
    if (section.read_unlock != nullptr &&
        !process_model_.isBeforeInFunction(inst, section.read_unlock)) {
      continue;
    }
    return &section;
  }

  return nullptr;
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findReadSideWithoutGracePeriod() const {
  std::vector<const Instruction *> result;

  for (const auto &section : read_sections_) {
    if (!section.has_sync) {
      result.push_back(section.read_lock);
    }
  }

  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelRCUAnalysis::findRCUConflicts() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> conflicts;
  std::vector<const KernelOperation *> writers;

  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind == OperationKind::RCU_ASSIGN) {
      writers.push_back(&op);
    }
  }

  for (const auto &section : read_sections_) {
    if (section.has_sync) {
      continue;
    }
    for (const KernelOperation *writer : writers) {
      if (writer->inst->getFunction() != section.function) {
        continue;
      }
      if (process_model_.isBeforeInFunction(section.read_lock, writer->inst) &&
          (section.read_unlock == nullptr ||
           process_model_.isBeforeInFunction(writer->inst, section.read_unlock))) {
        conflicts.emplace_back(section.read_lock, writer->inst);
      }
    }
  }

  rcu_diagnostics_["rcu_conflict_checks"] += writers.size();
  return conflicts;
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findRCUDoubleFree() const {
  std::vector<const Instruction *> result;
  std::map<const Value *, const Instruction *> callbacks_by_target;

  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind != OperationKind::RCU_CALL || op.rcu_sync == nullptr) {
      continue;
    }
    auto [it, inserted] = callbacks_by_target.emplace(op.rcu_sync, op.inst);
    if (!inserted) {
      result.push_back(op.inst);
    }
  }
  return result;
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findDerefAfterFree() const {
  std::vector<const Instruction *> result;
  std::map<const Value *, const Instruction *> release_points;

  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind == OperationKind::RCU_SYNC && op.rcu_sync != nullptr) {
      release_points[op.rcu_sync] = op.inst;
      continue;
    }

    if (op.kind != OperationKind::RCU_DEREFERENCE || op.rcu_sync == nullptr) {
      continue;
    }

    auto it = release_points.find(op.rcu_sync);
    if (it != release_points.end() &&
        it->second->getFunction() == op.inst->getFunction() &&
        process_model_.isBeforeInFunction(it->second, op.inst)) {
      result.push_back(op.inst);
    }
  }
  return result;
}

std::vector<const Instruction *>
LinuxKernelRCUAnalysis::findDerefInWrongSection() const {
  std::vector<const Instruction *> result;
  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind == OperationKind::RCU_DEREFERENCE &&
        !isWithinRCUSection(op.inst)) {
      result.push_back(op.inst);
    }
  }
  return result;
}

} // namespace kernel
