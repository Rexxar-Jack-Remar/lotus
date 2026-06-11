/** @file Hot.h @brief Hot code detection analysis based on profiling data. */
#pragma once

#include "Analysis/Loop/LoopStructure.h"

#include <cstdint>
#include <functional>
#include <unordered_map>

#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/BranchProbabilityInfo.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace lotus {
namespace analysis {
namespace profile {

class Hot {
public:
  using BFIProvider = std::function<llvm::BlockFrequencyInfo &(llvm::Function &)>;
  using BPIProvider = std::function<llvm::BranchProbabilityInfo &(llvm::Function &)>;

  Hot(llvm::Module &module, BFIProvider get_bfi, BPIProvider get_bpi);

  bool isAvailable(void) const;

  uint64_t getStaticInstructions(const llvm::Instruction *instruction) const;
  bool hasBeenExecuted(const llvm::Instruction *instruction) const;
  uint64_t getInvocations(const llvm::Instruction *instruction) const;
  uint64_t getSelfInstructions(const llvm::Instruction *instruction) const;
  uint64_t getTotalInstructions(const llvm::Instruction *instruction) const;
  double getDynamicTotalInstructionCoverage(
      const llvm::Instruction *instruction) const;

  uint64_t getStaticInstructions(const llvm::BasicBlock *basic_block) const;
  uint64_t getStaticInstructions(
      const llvm::BasicBlock *basic_block,
      const std::function<bool(const llvm::Instruction *)> &predicate) const;
  bool hasBeenExecuted(const llvm::BasicBlock *basic_block) const;
  uint64_t getInvocations(const llvm::BasicBlock *basic_block) const;
  uint64_t getSelfInstructions(const llvm::BasicBlock *basic_block) const;
  uint64_t getTotalInstructions(const llvm::BasicBlock *basic_block) const;

  uint64_t getStaticInstructions(const llvm::Loop *loop) const;
  uint64_t getStaticInstructions(
      const llvm::Loop *loop,
      const std::function<bool(const llvm::Instruction *)> &predicate) const;
  bool hasBeenExecuted(const llvm::Loop *loop) const;
  uint64_t getInvocations(const llvm::Loop *loop) const;
  uint64_t getIterations(const llvm::Loop *loop) const;
  uint64_t getSelfInstructions(const llvm::Loop *loop) const;
  uint64_t getTotalInstructions(const llvm::Loop *loop) const;
  double getDynamicTotalInstructionCoverage(const llvm::Loop *loop) const;
  double getAverageTotalInstructionsPerInvocation(const llvm::Loop *loop) const;
  double getAverageLoopIterationsPerInvocation(const llvm::Loop *loop) const;
  double getAverageTotalInstructionsPerIteration(const llvm::Loop *loop) const;

  uint64_t getStaticInstructions(
      const lotus::analysis::loop::LoopStructure *loop) const;
  uint64_t getStaticInstructions(
      const lotus::analysis::loop::LoopStructure *loop,
      const std::function<bool(const llvm::Instruction *)> &predicate) const;
  bool hasBeenExecuted(const lotus::analysis::loop::LoopStructure *loop) const;
  uint64_t getInvocations(const lotus::analysis::loop::LoopStructure *loop) const;
  uint64_t getIterations(const lotus::analysis::loop::LoopStructure *loop) const;
  uint64_t getSelfInstructions(
      const lotus::analysis::loop::LoopStructure *loop) const;
  uint64_t getTotalInstructions(
      const lotus::analysis::loop::LoopStructure *loop) const;
  double getDynamicTotalInstructionCoverage(
      const lotus::analysis::loop::LoopStructure *loop) const;
  double getAverageTotalInstructionsPerInvocation(
      const lotus::analysis::loop::LoopStructure *loop) const;
  double getAverageLoopIterationsPerInvocation(
      const lotus::analysis::loop::LoopStructure *loop) const;
  double getAverageTotalInstructionsPerIteration(
      const lotus::analysis::loop::LoopStructure *loop) const;

  uint64_t getStaticInstructions(const llvm::Function *function) const;
  uint64_t getStaticInstructions(
      const llvm::Function *function,
      const std::function<bool(const llvm::Instruction *)> &predicate) const;
  bool hasBeenExecuted(const llvm::Function *function) const;
  uint64_t getInvocations(const llvm::Function *function) const;
  uint64_t getSelfInstructions(const llvm::Function *function) const;
  uint64_t getTotalInstructions(const llvm::Function *function) const;
  double getDynamicTotalInstructionCoverage(const llvm::Function *function) const;

  bool hasBeenExecuted(void) const;
  uint64_t getInvocations(void) const;
  uint64_t getSelfInstructions(void) const;
  uint64_t getTotalInstructions(void) const;

  double getBranchFrequency(const llvm::BasicBlock *source,
                            const llvm::BasicBlock *target) const;

private:
  std::unordered_map<const llvm::BasicBlock *,
                     std::unordered_map<const llvm::BasicBlock *, double>>
      branch_probability_;
  std::unordered_map<const llvm::BasicBlock *, uint64_t> bb_invocations_;
  std::unordered_map<const llvm::Function *, uint64_t> function_invocations_;
  std::unordered_map<const llvm::Function *, uint64_t> function_self_insts_;
  std::unordered_map<const llvm::Function *, uint64_t> function_total_insts_;
  std::unordered_map<const llvm::Instruction *, uint64_t> instruction_total_insts_;
  uint64_t module_self_insts_ = 0;
  BFIProvider get_bfi_;
  BPIProvider get_bpi_;

  void analyzeProfiles(llvm::Module &module);
  void computeProgramInvocations(llvm::Module &module);
  void computeTotalInstructions(llvm::Module &module);
  void computeTotalInstructions(
      llvm::Function &function,
      std::unordered_map<const llvm::Function *, bool> &evaluation_stack);
  void setBasicBlockInvocations(const llvm::BasicBlock *basic_block,
                                uint64_t invocations);
  void setFunctionTotalInstructions(const llvm::Function *function,
                                    uint64_t total_instructions);
  bool isFunctionTotalInstructionsAvailable(
      const llvm::Function &function) const;
  void setBranchFrequency(const llvm::BasicBlock *source,
                          const llvm::BasicBlock *target,
                          double branch_frequency);
};

} // namespace profile
} // namespace analysis
} // namespace lotus
