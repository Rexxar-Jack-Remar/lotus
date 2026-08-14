/**
 * @file LinuxKernelLifetimeAnalysis.cpp
 * @brief Kernel object and callback lifetime analysis implementation.
 */

#include "Concurrency/LinuxKernel/LinuxKernelLifetimeAnalysis.h"

#include "Concurrency/LinuxKernel/LinuxKernelExecutionGraph.h"
#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <tuple>

#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace kernel {

namespace {

const Value *underlyingObject(const Value *value) {
  if (value == nullptr || !value->getType()->isPointerTy()) {
    return nullptr;
  }
  return getUnderlyingObject(value->stripPointerCasts());
}

bool instructionUsesOwner(const Instruction &instruction, const Value *owner) {
  const Value *owner_base = underlyingObject(owner);
  if (owner_base == nullptr) {
    return false;
  }
  for (const Use &operand : instruction.operands()) {
    const Value *value = operand.get();
    if (value->getType()->isPointerTy() &&
        underlyingObject(value) == owner_base) {
      return true;
    }
  }
  return false;
}

} // namespace

void LinuxKernelLifetimeAnalysis::analyze() {
  hazards_.clear();
  std::map<const Function *, std::unique_ptr<DominatorTree>> dominators;

  for (const KernelOperation &free : process_model_.getAllOperations()) {
    if ((free.kind != OperationKind::RCU_RECLAIM &&
         free.kind != OperationKind::MEMORY_FREE) ||
        free.memory_object == nullptr) {
      continue;
    }

    const Function *function = free.inst->getFunction();
    auto &dt = dominators[function];
    if (!dt) {
      dt = std::make_unique<DominatorTree>(*const_cast<Function *>(function));
    }
    for (const BasicBlock &block : *function) {
      for (const Instruction &candidate : block) {
        if (&candidate == free.inst || !dt->dominates(free.inst, &candidate) ||
            !instructionUsesOwner(candidate, free.memory_object)) {
          continue;
        }
        hazards_.push_back(
            {HazardKind::DIRECT_USE_AFTER_FREE, free.inst, &candidate});
      }
    }

    const Value *freed_base = underlyingObject(free.memory_object);
    for (const LinuxKernelExecutionGraph::Context &context :
         execution_graph_.getContexts()) {
      if (!context.explicit_concurrency || context.origin == nullptr ||
          underlyingObject(context.object) != freed_base ||
          context.origin->getFunction() != function ||
          !dt->dominates(context.origin, free.inst)) {
        continue;
      }

      bool synchronized = false;
      for (const LinuxKernelExecutionGraph::Edge &edge :
           execution_graph_.getEdges()) {
        if (edge.context == context.id && edge.synchronous &&
            edge.operation != nullptr &&
            edge.operation->getFunction() == function &&
            dt->dominates(context.origin, edge.operation) &&
            dt->dominates(edge.operation, free.inst)) {
          synchronized = true;
          break;
        }
      }
      if (!synchronized) {
        hazards_.push_back(
            {HazardKind::ASYNC_CALLBACK_AFTER_FREE, free.inst, context.origin});
      }
    }
  }

  llvm::sort(hazards_, [](const Hazard &lhs, const Hazard &rhs) {
    return std::tie(lhs.kind, lhs.free_inst, lhs.use_or_submit_inst) <
           std::tie(rhs.kind, rhs.free_inst, rhs.use_or_submit_inst);
  });
  hazards_.erase(std::unique(hazards_.begin(), hazards_.end(),
                             [](const Hazard &lhs, const Hazard &rhs) {
                               return lhs.kind == rhs.kind &&
                                      lhs.free_inst == rhs.free_inst &&
                                      lhs.use_or_submit_inst ==
                                          rhs.use_or_submit_inst;
                             }),
                 hazards_.end());
}

std::vector<const Instruction *>
LinuxKernelLifetimeAnalysis::findUseAfterFree() const {
  std::vector<const Instruction *> result;
  for (const Hazard &hazard : hazards_) {
    if (hazard.kind == HazardKind::DIRECT_USE_AFTER_FREE) {
      result.push_back(hazard.use_or_submit_inst);
    }
  }
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelLifetimeAnalysis::findAsyncLifetimeHazards() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> result;
  for (const Hazard &hazard : hazards_) {
    if (hazard.kind == HazardKind::ASYNC_CALLBACK_AFTER_FREE) {
      result.emplace_back(hazard.free_inst, hazard.use_or_submit_inst);
    }
  }
  return result;
}

} // namespace kernel
