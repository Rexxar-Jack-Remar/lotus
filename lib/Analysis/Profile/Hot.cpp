#include "Analysis/Profile/Hot.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {
namespace profile {

Hot::Hot(llvm::Module &module, BFIProvider get_bfi, BPIProvider get_bpi)
    : get_bfi_(std::move(get_bfi)), get_bpi_(std::move(get_bpi)) {
  analyzeProfiles(module);
}

bool Hot::isAvailable(void) const { return hasBeenExecuted(); }

void Hot::analyzeProfiles(llvm::Module &module) {
  for (auto &function : module) {
    if (function.empty()) {
      continue;
    }

    auto &bfi = get_bfi_(function);
    auto &bpi = get_bpi_(function);

    for (auto &basic_block : function) {
      auto count = bfi.getBlockProfileCount(&basic_block);
      setBasicBlockInvocations(&basic_block, count.hasValue() ? count.getValue() : 0);

      for (auto *successor : llvm::successors(&basic_block)) {
        auto probability = bpi.getEdgeProbability(&basic_block, successor);
        if (probability.isUnknown()) {
          continue;
        }
        auto numerator = static_cast<double>(probability.getNumerator());
        auto denominator = static_cast<double>(probability.getDenominator());
        setBranchFrequency(&basic_block, successor, numerator / denominator);
      }
    }
  }

  computeProgramInvocations(module);
}

uint64_t Hot::getStaticInstructions(
    const llvm::Instruction *instruction) const {
  assert(instruction != nullptr);
  return 1;
}

bool Hot::hasBeenExecuted(const llvm::Instruction *instruction) const {
  return getInvocations(instruction) > 0;
}

uint64_t Hot::getInvocations(const llvm::Instruction *instruction) const {
  assert(instruction != nullptr);
  auto invocations = getInvocations(instruction->getParent());

  auto *call = llvm::dyn_cast<llvm::CallBase>(instruction);
  if (!call) {
    return invocations;
  }

  auto *callee = call->getCalledFunction();
  if (!callee || callee->empty()) {
    return invocations;
  }

  return std::min(invocations, getInvocations(callee));
}

uint64_t Hot::getSelfInstructions(const llvm::Instruction *instruction) const {
  return getInvocations(instruction);
}

uint64_t Hot::getTotalInstructions(const llvm::Instruction *instruction) const {
  auto it = instruction_total_insts_.find(instruction);
  if (it != instruction_total_insts_.end()) {
    return it->second;
  }
  return getInvocations(instruction);
}

double Hot::getDynamicTotalInstructionCoverage(
    const llvm::Instruction *instruction) const {
  auto module_total = getTotalInstructions();
  if (module_total == 0) {
    return 0;
  }
  return static_cast<double>(getTotalInstructions(instruction)) /
         static_cast<double>(module_total);
}

uint64_t Hot::getStaticInstructions(
    const llvm::BasicBlock *basic_block) const {
  assert(basic_block != nullptr);
  return static_cast<uint64_t>(std::distance(basic_block->begin(),
                                             basic_block->end()));
}

uint64_t Hot::getStaticInstructions(
    const llvm::BasicBlock *basic_block,
    const std::function<bool(const llvm::Instruction *)> &predicate) const {
  assert(basic_block != nullptr);
  uint64_t count = 0;
  for (const auto &instruction : *basic_block) {
    if (predicate(&instruction)) {
      ++count;
    }
  }
  return count;
}

bool Hot::hasBeenExecuted(const llvm::BasicBlock *basic_block) const {
  return getInvocations(basic_block) > 0;
}

uint64_t Hot::getInvocations(const llvm::BasicBlock *basic_block) const {
  assert(basic_block != nullptr);
  auto it = bb_invocations_.find(basic_block);
  return it == bb_invocations_.end() ? 0 : it->second;
}

uint64_t Hot::getSelfInstructions(const llvm::BasicBlock *basic_block) const {
  return getInvocations(basic_block) * getStaticInstructions(basic_block);
}

uint64_t Hot::getTotalInstructions(const llvm::BasicBlock *basic_block) const {
  assert(basic_block != nullptr);
  uint64_t total = 0;
  for (const auto &instruction : *basic_block) {
    total += getTotalInstructions(&instruction);
  }
  return total;
}

uint64_t Hot::getStaticInstructions(const llvm::Loop *loop) const {
  assert(loop != nullptr);
  uint64_t count = 0;
  for (auto *basic_block : loop->blocks()) {
    count += getStaticInstructions(basic_block);
  }
  return count;
}

uint64_t Hot::getStaticInstructions(
    const llvm::Loop *loop,
    const std::function<bool(const llvm::Instruction *)> &predicate) const {
  assert(loop != nullptr);
  uint64_t count = 0;
  for (auto *basic_block : loop->blocks()) {
    count += getStaticInstructions(basic_block, predicate);
  }
  return count;
}

bool Hot::hasBeenExecuted(const llvm::Loop *loop) const {
  return getInvocations(loop) > 0;
}

uint64_t Hot::getInvocations(const llvm::Loop *loop) const {
  assert(loop != nullptr);
  auto *preheader = loop->getLoopPreheader();
  if (preheader) {
    return getInvocations(preheader);
  }
  return getInvocations(loop->getHeader());
}

uint64_t Hot::getIterations(const llvm::Loop *loop) const {
  assert(loop != nullptr);
  auto *header = loop->getHeader();
  auto header_invocations = getInvocations(header);
  uint64_t successor_invocations = 0;
  for (auto *successor : llvm::successors(header)) {
    if (loop->contains(successor)) {
      successor_invocations += getInvocations(successor);
    }
  }
  return header_invocations == successor_invocations ? header_invocations
                                                     : header_invocations - 1;
}

uint64_t Hot::getSelfInstructions(const llvm::Loop *loop) const {
  uint64_t total = 0;
  for (auto *basic_block : loop->blocks()) {
    total += getSelfInstructions(basic_block);
  }
  return total;
}

uint64_t Hot::getTotalInstructions(const llvm::Loop *loop) const {
  uint64_t total = 0;
  for (auto *basic_block : loop->blocks()) {
    total += getTotalInstructions(basic_block);
  }
  return total;
}

double Hot::getDynamicTotalInstructionCoverage(const llvm::Loop *loop) const {
  auto module_total = getTotalInstructions();
  if (module_total == 0) {
    return 0;
  }
  return static_cast<double>(getTotalInstructions(loop)) /
         static_cast<double>(module_total);
}

double Hot::getAverageTotalInstructionsPerInvocation(
    const llvm::Loop *loop) const {
  auto invocations = getInvocations(loop);
  if (invocations == 0) {
    return 0;
  }
  return static_cast<double>(getTotalInstructions(loop)) /
         static_cast<double>(invocations);
}

double Hot::getAverageLoopIterationsPerInvocation(const llvm::Loop *loop) const {
  auto invocations = getInvocations(loop);
  if (invocations == 0) {
    return 0;
  }
  return static_cast<double>(getIterations(loop)) /
         static_cast<double>(invocations);
}

double Hot::getAverageTotalInstructionsPerIteration(const llvm::Loop *loop) const {
  auto iterations = getAverageLoopIterationsPerInvocation(loop);
  if (iterations == 0) {
    return 0;
  }
  return getAverageTotalInstructionsPerInvocation(loop) / iterations;
}

uint64_t Hot::getStaticInstructions(
    const lotus::analysis::loop::LoopStructure *loop) const {
  assert(loop != nullptr);
  uint64_t count = 0;
  for (auto *basic_block : loop->getBasicBlocks()) {
    count += getStaticInstructions(basic_block);
  }
  return count;
}

uint64_t Hot::getStaticInstructions(
    const lotus::analysis::loop::LoopStructure *loop,
    const std::function<bool(const llvm::Instruction *)> &predicate) const {
  assert(loop != nullptr);
  uint64_t count = 0;
  for (auto *basic_block : loop->getBasicBlocks()) {
    count += getStaticInstructions(basic_block, predicate);
  }
  return count;
}

bool Hot::hasBeenExecuted(
    const lotus::analysis::loop::LoopStructure *loop) const {
  return getInvocations(loop) > 0;
}

uint64_t Hot::getInvocations(
    const lotus::analysis::loop::LoopStructure *loop) const {
  assert(loop != nullptr);
  auto *preheader = loop->getPreHeader();
  return preheader ? getInvocations(preheader) : getInvocations(loop->getHeader());
}

uint64_t Hot::getIterations(
    const lotus::analysis::loop::LoopStructure *loop) const {
  assert(loop != nullptr);
  auto *header = loop->getHeader();
  auto header_invocations = getInvocations(header);
  uint64_t successor_invocations = 0;
  for (auto *successor : llvm::successors(header)) {
    if (loop->isIncluded(successor)) {
      successor_invocations += getInvocations(successor);
    }
  }
  return header_invocations == successor_invocations ? header_invocations
                                                     : header_invocations - 1;
}

uint64_t Hot::getSelfInstructions(
    const lotus::analysis::loop::LoopStructure *loop) const {
  uint64_t total = 0;
  for (auto *basic_block : loop->getBasicBlocks()) {
    total += getSelfInstructions(basic_block);
  }
  return total;
}

uint64_t Hot::getTotalInstructions(
    const lotus::analysis::loop::LoopStructure *loop) const {
  uint64_t total = 0;
  for (auto *basic_block : loop->getBasicBlocks()) {
    total += getTotalInstructions(basic_block);
  }
  return total;
}

double Hot::getDynamicTotalInstructionCoverage(
    const lotus::analysis::loop::LoopStructure *loop) const {
  auto module_total = getTotalInstructions();
  if (module_total == 0) {
    return 0;
  }
  return static_cast<double>(getTotalInstructions(loop)) /
         static_cast<double>(module_total);
}

double Hot::getAverageTotalInstructionsPerInvocation(
    const lotus::analysis::loop::LoopStructure *loop) const {
  auto invocations = getInvocations(loop);
  if (invocations == 0) {
    return 0;
  }
  return static_cast<double>(getTotalInstructions(loop)) /
         static_cast<double>(invocations);
}

double Hot::getAverageLoopIterationsPerInvocation(
    const lotus::analysis::loop::LoopStructure *loop) const {
  auto invocations = getInvocations(loop);
  if (invocations == 0) {
    return 0;
  }
  return static_cast<double>(getIterations(loop)) /
         static_cast<double>(invocations);
}

double Hot::getAverageTotalInstructionsPerIteration(
    const lotus::analysis::loop::LoopStructure *loop) const {
  auto iterations = getAverageLoopIterationsPerInvocation(loop);
  if (iterations == 0) {
    return 0;
  }
  return getAverageTotalInstructionsPerInvocation(loop) / iterations;
}

uint64_t Hot::getStaticInstructions(const llvm::Function *function) const {
  assert(function != nullptr);
  uint64_t total = 0;
  for (const auto &basic_block : *function) {
    total += getStaticInstructions(&basic_block);
  }
  return total;
}

uint64_t Hot::getStaticInstructions(
    const llvm::Function *function,
    const std::function<bool(const llvm::Instruction *)> &predicate) const {
  assert(function != nullptr);
  uint64_t total = 0;
  for (const auto &basic_block : *function) {
    total += getStaticInstructions(&basic_block, predicate);
  }
  return total;
}

bool Hot::hasBeenExecuted(const llvm::Function *function) const {
  return getInvocations(function) > 0;
}

uint64_t Hot::getInvocations(const llvm::Function *function) const {
  assert(function != nullptr);
  auto it = function_invocations_.find(function);
  return it == function_invocations_.end() ? 0 : it->second;
}

uint64_t Hot::getSelfInstructions(const llvm::Function *function) const {
  auto it = function_self_insts_.find(function);
  return it == function_self_insts_.end() ? 0 : it->second;
}

uint64_t Hot::getTotalInstructions(const llvm::Function *function) const {
  auto it = function_total_insts_.find(function);
  return it == function_total_insts_.end() ? 0 : it->second;
}

double Hot::getDynamicTotalInstructionCoverage(
    const llvm::Function *function) const {
  auto module_total = getTotalInstructions();
  if (module_total == 0) {
    return 0;
  }
  return static_cast<double>(getTotalInstructions(function)) /
         static_cast<double>(module_total);
}

bool Hot::hasBeenExecuted(void) const { return getSelfInstructions() > 0; }

uint64_t Hot::getInvocations(void) const { return hasBeenExecuted() ? 1 : 0; }

uint64_t Hot::getSelfInstructions(void) const { return module_self_insts_; }

uint64_t Hot::getTotalInstructions(void) const { return getSelfInstructions(); }

double Hot::getBranchFrequency(const llvm::BasicBlock *source,
                               const llvm::BasicBlock *target) const {
  auto source_it = branch_probability_.find(source);
  if (source_it == branch_probability_.end()) {
    return 0;
  }
  auto target_it = source_it->second.find(target);
  return target_it == source_it->second.end() ? 0 : target_it->second;
}

void Hot::computeProgramInvocations(llvm::Module &module) {
  for (const auto &[basic_block, invocations] : bb_invocations_) {
    module_self_insts_ += invocations * getStaticInstructions(basic_block);
  }

  for (const auto &[function, invocations] : function_invocations_) {
    (void)invocations;
    uint64_t total = 0;
    for (const auto &basic_block : *function) {
      total += getInvocations(&basic_block) * getStaticInstructions(&basic_block);
    }
    function_self_insts_[function] = total;
  }

  computeTotalInstructions(module);
}

void Hot::computeTotalInstructions(llvm::Module &module) {
  for (auto &function : module) {
    if (function.empty()) {
      continue;
    }
    std::unordered_map<const llvm::Function *, bool> evaluation_stack;
    computeTotalInstructions(function, evaluation_stack);
  }

  for (auto &function : module) {
    if (function.empty() || !hasBeenExecuted(&function)) {
      continue;
    }

    auto total = getTotalInstructions(&function);
    auto invocations = getInvocations(&function);
    if (invocations == 0) {
      continue;
    }
    auto per_invocation = total / invocations;
    auto leftover = total;
    const llvm::Instruction *last_caller = nullptr;

    for (auto &use : function.uses()) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(use.getUser());
      if (!call || !hasBeenExecuted(call)) {
        continue;
      }
      auto *caller = static_cast<const llvm::Instruction *>(call);
      last_caller = caller;
      auto caller_total = per_invocation * getInvocations(caller);
      instruction_total_insts_[caller] = caller_total + 1;
      leftover -= caller_total;
    }

    if (leftover > 0 && last_caller) {
      instruction_total_insts_[last_caller] += leftover;
    }
  }
}

void Hot::computeTotalInstructions(
    llvm::Function &function,
    std::unordered_map<const llvm::Function *, bool> &evaluation_stack) {
  evaluation_stack[&function] = true;

  if (!hasBeenExecuted(&function)) {
    setFunctionTotalInstructions(&function, 0);
    return;
  }

  uint64_t total = 0;
  for (auto &instruction : llvm::instructions(function)) {
    if (!hasBeenExecuted(&instruction)) {
      continue;
    }

    auto instruction_invocations = getInvocations(&instruction);
    total += instruction_invocations;

    auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call) {
      continue;
    }

    auto *callee = call->getCalledFunction();
    if (!callee || callee->empty() || !hasBeenExecuted(callee)) {
      continue;
    }

    uint64_t callee_total = 0;
    if (!isFunctionTotalInstructionsAvailable(*callee)) {
      if (evaluation_stack[callee]) {
        callee_total = 1;
      } else {
        computeTotalInstructions(*callee, evaluation_stack);
        callee_total = getTotalInstructions(callee);
      }
    } else {
      callee_total = getTotalInstructions(callee);
    }

    if (callee_total == 0 || getInvocations(callee) == 0) {
      continue;
    }
    total += (callee_total / getInvocations(callee)) * instruction_invocations;
  }

  setFunctionTotalInstructions(&function, total);
}

void Hot::setBasicBlockInvocations(const llvm::BasicBlock *basic_block,
                                   uint64_t invocations) {
  assert(basic_block != nullptr);
  auto *function = basic_block->getParent();
  if (&function->getEntryBlock() == basic_block) {
    function_invocations_[function] = invocations;
  }
  bb_invocations_[basic_block] = invocations;
}

void Hot::setFunctionTotalInstructions(const llvm::Function *function,
                                       uint64_t total_instructions) {
  function_total_insts_[function] = total_instructions;
}

bool Hot::isFunctionTotalInstructionsAvailable(
    const llvm::Function &function) const {
  return function_total_insts_.find(&function) != function_total_insts_.end();
}

void Hot::setBranchFrequency(const llvm::BasicBlock *source,
                             const llvm::BasicBlock *target,
                             double branch_frequency) {
  branch_probability_[source][target] = branch_frequency;
}

} // namespace profile
} // namespace analysis
} // namespace lotus
