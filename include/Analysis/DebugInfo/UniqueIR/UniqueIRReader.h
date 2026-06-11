/** @file UniqueIRReader.h @brief Reader for unique IR encoded data. */
#pragma once

#include "Analysis/DebugInfo/UniqueIR/UniqueIRConstants.h"

#include <functional>
#include <optional>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

namespace lotus {

class UniqueIRReader {
public:
  static std::optional<llvm::Constant *> getInstructionConstID(
      const llvm::Instruction *instruction);
  static std::optional<llvm::Constant *> getBasicBlockConstID(
      const llvm::BasicBlock *basic_block);
  static std::optional<llvm::Constant *> getFunctionConstID(
      const llvm::Function *function);
  static std::optional<llvm::Constant *> getModuleConstID(
      const llvm::Module *module);
  static std::optional<llvm::Constant *> getLoopConstID(const llvm::Loop *loop);

  static std::optional<UniqueIRID> getInstructionID(
      const llvm::Instruction *instruction);
  static std::optional<UniqueIRID> getBasicBlockID(
      const llvm::BasicBlock *basic_block);
  static std::optional<UniqueIRID> getFunctionID(const llvm::Function *function);
  static std::optional<UniqueIRID> getModuleID(const llvm::Module *module);
  static std::optional<UniqueIRID> getLoopID(const llvm::Loop *loop);

private:
  static std::optional<llvm::Constant *> getConstFromNode(
      const llvm::MDNode *node,
      unsigned operand);
  static std::optional<llvm::Constant *> getLoopIDConstFromNode(
      const llvm::MDNode *node);
  static std::optional<UniqueIRID> getID(const llvm::Constant *constant);
};

} // namespace lotus
