/**
 * @file LinuxKernelMemoryModel.cpp
 * @brief LKMM-oriented kernel memory-event extraction.
 */

#include "Concurrency/LinuxKernel/LinuxKernelMemoryModel.h"

#include "Concurrency/LinuxKernel/LinuxKernelExecutionGraph.h"
#include "Concurrency/LinuxKernel/LinuxKernelLockAnalysis.h"
#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace kernel {

namespace {

LinuxKernelMemoryModel::MemoryOrder orderForKernel(KernelMemoryOrder order) {
  using MemoryOrder = LinuxKernelMemoryModel::MemoryOrder;
  switch (order) {
  case KernelMemoryOrder::NONE:
    return MemoryOrder::NONE;
  case KernelMemoryOrder::RELAXED:
    return MemoryOrder::RELAXED;
  case KernelMemoryOrder::ACQUIRE:
    return MemoryOrder::ACQUIRE;
  case KernelMemoryOrder::RELEASE:
    return MemoryOrder::RELEASE;
  case KernelMemoryOrder::ACQ_REL:
    return MemoryOrder::ACQ_REL;
  case KernelMemoryOrder::FULL:
    return MemoryOrder::FULL;
  case KernelMemoryOrder::COMPILER:
    return MemoryOrder::COMPILER;
  case KernelMemoryOrder::UNKNOWN:
    return MemoryOrder::UNKNOWN;
  }
  return MemoryOrder::UNKNOWN;
}

bool isReadEvent(const LinuxKernelMemoryModel::Event &event) {
  return event.read;
}

bool isWriteEvent(const LinuxKernelMemoryModel::Event &event) {
  return event.write;
}

LinuxKernelMemoryModel::MemoryOrder orderForLLVM(AtomicOrdering order) {
  switch (order) {
  case AtomicOrdering::Unordered:
  case AtomicOrdering::Monotonic:
    return LinuxKernelMemoryModel::MemoryOrder::RELAXED;
  case AtomicOrdering::Acquire:
    return LinuxKernelMemoryModel::MemoryOrder::ACQUIRE;
  case AtomicOrdering::Release:
    return LinuxKernelMemoryModel::MemoryOrder::RELEASE;
  case AtomicOrdering::AcquireRelease:
    return LinuxKernelMemoryModel::MemoryOrder::ACQ_REL;
  case AtomicOrdering::SequentiallyConsistent:
    return LinuxKernelMemoryModel::MemoryOrder::FULL;
  case AtomicOrdering::NotAtomic:
    return LinuxKernelMemoryModel::MemoryOrder::NONE;
  }
  return LinuxKernelMemoryModel::MemoryOrder::UNKNOWN;
}

bool valueDependsOn(const Value *value, const Instruction *source,
                    std::set<const Value *> &visited) {
  if (value == nullptr || source == nullptr || !visited.insert(value).second) {
    return false;
  }
  if (value == source) {
    return true;
  }
  const auto *user = dyn_cast<User>(value);
  if (user == nullptr) {
    return false;
  }
  for (const Use &operand : user->operands()) {
    if (valueDependsOn(operand.get(), source, visited)) {
      return true;
    }
  }
  return false;
}

bool valueDependsOn(const Value *value, const Instruction *source) {
  std::set<const Value *> visited;
  return valueDependsOn(value, source, visited);
}

} // namespace

void LinuxKernelMemoryModel::addEvent(const Event &source) {
  Event event = source;
  event.id = events_.size();
  events_.push_back(event);
  events_by_inst_[event.inst].push_back(event.id);
}

void LinuxKernelMemoryModel::analyze() {
  events_.clear();
  relations_.clear();
  events_by_inst_.clear();
  protecting_locks_.clear();

  for (const Function &function : process_model_.getModule()) {
    EventID previous = 0;
    bool has_previous = false;
    for (const Instruction &instruction : instructions(function)) {
      const size_t first_new_event = events_.size();

      if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
        const bool marked = load->isVolatile() || load->isAtomic();
        addEvent({0, &instruction,
                  process_model_.canonicalizeValue(load->getPointerOperand()),
                  marked ? EventKind::MARKED_READ : EventKind::PLAIN_READ,
                  load->isAtomic() ? orderForLLVM(load->getOrdering())
                                   : MemoryOrder::NONE,
                  true, false, !marked});
      } else if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
        const bool marked = store->isVolatile() || store->isAtomic();
        addEvent({0, &instruction,
                  process_model_.canonicalizeValue(store->getPointerOperand()),
                  marked ? EventKind::MARKED_WRITE : EventKind::PLAIN_WRITE,
                  store->isAtomic() ? orderForLLVM(store->getOrdering())
                                    : MemoryOrder::NONE,
                  false, true, !marked});
      } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(&instruction)) {
        addEvent({0, &instruction,
                  process_model_.canonicalizeValue(rmw->getPointerOperand()),
                  EventKind::ATOMIC_RMW, orderForLLVM(rmw->getOrdering()), true,
                  true, false});
      } else if (const auto *cmpxchg =
                     dyn_cast<AtomicCmpXchgInst>(&instruction)) {
        addEvent(
            {0, &instruction,
             process_model_.canonicalizeValue(cmpxchg->getPointerOperand()),
             EventKind::ATOMIC_RMW, orderForLLVM(cmpxchg->getSuccessOrdering()),
             true, true, false});
      } else if (const auto *fence = dyn_cast<FenceInst>(&instruction)) {
        addEvent({0, &instruction, nullptr, EventKind::FENCE,
                  orderForLLVM(fence->getOrdering()), false, false, false});
      }

      for (const KernelOperation *op :
           process_model_.getOperationsForInstruction(&instruction)) {
        Event event;
        event.inst = &instruction;
        event.order = orderForKernel(op->memory_order);
        switch (op->kind) {
        case OperationKind::ATOMIC_READ:
          event.object = op->atomic_var;
          event.kind = EventKind::MARKED_READ;
          event.read = true;
          addEvent(event);
          break;
        case OperationKind::ATOMIC_WRITE:
          event.object = op->atomic_var;
          event.kind = EventKind::MARKED_WRITE;
          event.write = true;
          addEvent(event);
          break;
        case OperationKind::ATOMIC_RMW:
          event.object = op->atomic_var;
          event.kind = EventKind::ATOMIC_RMW;
          event.read = true;
          event.write = true;
          addEvent(event);
          break;
        case OperationKind::MEMORY_BARRIER:
          event.kind = EventKind::FENCE;
          addEvent(event);
          break;
        case OperationKind::LOCK_ACQUIRE:
        case OperationKind::LOCK_TRY:
          event.object = op->lock;
          event.kind = EventKind::LOCK_ACQUIRE;
          event.order = MemoryOrder::ACQUIRE;
          addEvent(event);
          break;
        case OperationKind::LOCK_RELEASE:
          event.object = op->lock;
          event.kind = EventKind::LOCK_RELEASE;
          event.order = MemoryOrder::RELEASE;
          addEvent(event);
          break;
        case OperationKind::RCU_READ_LOCK:
        case OperationKind::RCU_READ_UNLOCK:
        case OperationKind::RCU_SYNC:
        case OperationKind::RCU_CALL:
        case OperationKind::RCU_BARRIER:
        case OperationKind::RCU_ASSIGN:
        case OperationKind::RCU_DEREFERENCE:
          event.object = op->rcu_target;
          event.kind = EventKind::RCU;
          addEvent(event);
          break;
        default:
          break;
        }
      }

      for (size_t index = first_new_event; index < events_.size(); ++index) {
        if (has_previous) {
          relations_.push_back(
              {previous, events_[index].id, RelationKind::PROGRAM_ORDER});
        }
        previous = events_[index].id;
        has_previous = true;
      }
    }
  }

  std::set<std::tuple<EventID, EventID, RelationKind>> dependency_relations;
  for (const Function &function : process_model_.getModule()) {
    if (function.isDeclaration()) {
      continue;
    }
    DominatorTree dominators(*const_cast<Function *>(&function));
    std::vector<const Event *> function_events;
    for (const Event &event : events_) {
      if (event.inst->getFunction() == &function) {
        function_events.push_back(&event);
      }
    }

    for (const Event *source : function_events) {
      if (!source->read) {
        continue;
      }
      for (const Event *target : function_events) {
        if (source == target ||
            !dominators.dominates(source->inst, target->inst)) {
          continue;
        }

        const Value *address = nullptr;
        const Value *data = nullptr;
        if (const auto *load = dyn_cast<LoadInst>(target->inst)) {
          address = load->getPointerOperand();
        } else if (const auto *store = dyn_cast<StoreInst>(target->inst)) {
          address = store->getPointerOperand();
          data = store->getValueOperand();
        } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(target->inst)) {
          address = rmw->getPointerOperand();
          data = rmw->getValOperand();
        } else if (const auto *cmpxchg =
                       dyn_cast<AtomicCmpXchgInst>(target->inst)) {
          address = cmpxchg->getPointerOperand();
          data = cmpxchg->getNewValOperand();
        }
        if (valueDependsOn(address, source->inst)) {
          dependency_relations.emplace(source->id, target->id,
                                       RelationKind::ADDRESS_DEPENDENCY);
        }
        if (valueDependsOn(data, source->inst)) {
          dependency_relations.emplace(source->id, target->id,
                                       RelationKind::DATA_DEPENDENCY);
        }

        for (const BasicBlock &block : function) {
          const auto *branch = dyn_cast<BranchInst>(block.getTerminator());
          if (branch == nullptr || !branch->isConditional() ||
              !dominators.dominates(branch, target->inst) ||
              !valueDependsOn(branch->getCondition(), source->inst)) {
            continue;
          }
          const bool true_controls = dominators.dominates(
              branch->getSuccessor(0), target->inst->getParent());
          const bool false_controls = dominators.dominates(
              branch->getSuccessor(1), target->inst->getParent());
          if (true_controls != false_controls) {
            dependency_relations.emplace(source->id, target->id,
                                         RelationKind::CONTROL_DEPENDENCY);
          }
        }
      }
    }
  }
  for (const auto &relation : dependency_relations) {
    relations_.push_back(
        {std::get<0>(relation), std::get<1>(relation), std::get<2>(relation)});
  }
  computeProtectingLocks();
}

void LinuxKernelMemoryModel::computeProtectingLocks() {
  std::map<const Function *, std::unique_ptr<DominatorTree>> dominators;
  std::map<const Function *, std::unique_ptr<PostDominatorTree>>
      post_dominators;
  for (const auto &entry : events_by_inst_) {
    const Instruction *instruction = entry.first;
    const Function *function = instruction->getFunction();
    auto &dt = dominators[function];
    auto &pdt = post_dominators[function];
    if (!dt) {
      dt = std::make_unique<DominatorTree>(*const_cast<Function *>(function));
      pdt = std::make_unique<PostDominatorTree>();
      pdt->recalculate(*const_cast<Function *>(function));
    }
    auto &locks = protecting_locks_[instruction];
    for (const LinuxKernelLockAnalysis::LockRegion &region :
         lock_analysis_.getLockRegions()) {
      if (region.acquire_inst->getFunction() == function &&
          dt->dominates(region.acquire_inst, instruction) &&
          pdt->dominates(region.release_inst, instruction)) {
        locks.push_back(region.lock);
      }
    }
    llvm::sort(locks);
    locks.erase(std::unique(locks.begin(), locks.end()), locks.end());
  }
}

bool LinuxKernelMemoryModel::shareProtectingLock(const Instruction *lhs,
                                                 const Instruction *rhs) const {
  auto lhs_found = protecting_locks_.find(lhs);
  auto rhs_found = protecting_locks_.find(rhs);
  if (lhs_found == protecting_locks_.end() ||
      rhs_found == protecting_locks_.end()) {
    return false;
  }
  const std::vector<LockID> &lhs_locks = lhs_found->second;
  const std::vector<LockID> &rhs_locks = rhs_found->second;
  for (LockID lhs_lock : lhs_locks) {
    for (LockID rhs_lock : rhs_locks) {
      if (process_model_.mayAlias(lhs_lock, rhs_lock)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelMemoryModel::findDataRaceCandidates() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> result;
  std::map<const Value *, std::vector<const Event *>> by_object;
  for (const Event &event : events_) {
    if (event.object != nullptr &&
        (isReadEvent(event) || isWriteEvent(event))) {
      by_object[event.object].push_back(&event);
    }
  }

  std::vector<const Value *> objects;
  std::map<const Value *, size_t> object_index;
  for (const auto &bucket : by_object) {
    object_index[bucket.first] = objects.size();
    objects.push_back(bucket.first);
  }
  std::vector<size_t> parent(objects.size());
  for (size_t index = 0; index < parent.size(); ++index) {
    parent[index] = index;
  }
  std::function<size_t(size_t)> findRoot = [&](size_t index) {
    if (parent[index] != index) {
      parent[index] = findRoot(parent[index]);
    }
    return parent[index];
  };
  auto unite = [&](size_t lhs, size_t rhs) {
    lhs = findRoot(lhs);
    rhs = findRoot(rhs);
    if (lhs != rhs) {
      parent[rhs] = lhs;
    }
  };
  for (size_t index = 0; index < objects.size(); ++index) {
    std::vector<const Value *> aliases;
    if (!process_model_.getAliasSet(objects[index], aliases)) {
      continue;
    }
    for (const Value *alias : aliases) {
      const Value *canonical = process_model_.canonicalizeValue(alias);
      auto found = object_index.find(canonical);
      if (found != object_index.end()) {
        unite(index, found->second);
      }
    }
  }

  std::map<size_t, std::vector<const Event *>> alias_buckets;
  for (size_t index = 0; index < objects.size(); ++index) {
    auto found = by_object.find(objects[index]);
    alias_buckets[findRoot(index)].insert(alias_buckets[findRoot(index)].end(),
                                          found->second.begin(),
                                          found->second.end());
  }

  for (const auto &bucket : alias_buckets) {
    const auto &accesses = bucket.second;
    for (size_t lhs_index = 0; lhs_index < accesses.size(); ++lhs_index) {
      const Event &lhs = *accesses[lhs_index];
      const size_t rhs_begin = lhs.write ? lhs_index : lhs_index + 1;
      for (size_t rhs_index = rhs_begin; rhs_index < accesses.size();
           ++rhs_index) {
        const Event &rhs = *accesses[rhs_index];
        if ((!lhs.write && !rhs.write) || (!lhs.plain && !rhs.plain) ||
            !execution_graph_.mayHappenInParallel(lhs.inst, rhs.inst) ||
            shareProtectingLock(lhs.inst, rhs.inst)) {
          continue;
        }
        result.emplace_back(lhs.inst, rhs.inst);
      }
    }
  }
  llvm::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

} // namespace kernel
