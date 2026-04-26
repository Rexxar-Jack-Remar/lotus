#include "Analysis/DebugInfo/UniqueIR/UniqueIRReader.h"

#include <llvm/IR/Constants.h>

namespace lotus {

std::optional<llvm::Constant *> UniqueIRReader::getInstructionConstID(
    const llvm::Instruction *instruction) {
  if (!instruction) {
    return std::nullopt;
  }
  return getConstFromNode(
      instruction->getMetadata(UniqueIRConstants::InstructionID), 0);
}

std::optional<llvm::Constant *> UniqueIRReader::getBasicBlockConstID(
    const llvm::BasicBlock *basic_block) {
  if (!basic_block || basic_block->empty()) {
    return std::nullopt;
  }
  return getConstFromNode(
      basic_block->front().getMetadata(UniqueIRConstants::BasicBlockID), 0);
}

std::optional<llvm::Constant *> UniqueIRReader::getFunctionConstID(
    const llvm::Function *function) {
  if (!function) {
    return std::nullopt;
  }
  return getConstFromNode(function->getMetadata(UniqueIRConstants::FunctionID),
                          0);
}

std::optional<llvm::Constant *> UniqueIRReader::getModuleConstID(
    const llvm::Module *module) {
  if (!module) {
    return std::nullopt;
  }
  auto *named = module->getNamedMetadata(UniqueIRConstants::ModuleID);
  if (!named || named->getNumOperands() != 1) {
    return std::nullopt;
  }
  return getConstFromNode(named->getOperand(0), 0);
}

std::optional<llvm::Constant *> UniqueIRReader::getLoopConstID(
    const llvm::Loop *loop) {
  if (!loop) {
    return std::nullopt;
  }
  return getLoopIDConstFromNode(loop->getLoopID());
}

std::optional<UniqueIRID> UniqueIRReader::getInstructionID(
    const llvm::Instruction *instruction) {
  auto constant = getInstructionConstID(instruction);
  return constant ? getID(*constant) : std::nullopt;
}

std::optional<UniqueIRID> UniqueIRReader::getBasicBlockID(
    const llvm::BasicBlock *basic_block) {
  auto constant = getBasicBlockConstID(basic_block);
  return constant ? getID(*constant) : std::nullopt;
}

std::optional<UniqueIRID> UniqueIRReader::getFunctionID(
    const llvm::Function *function) {
  auto constant = getFunctionConstID(function);
  return constant ? getID(*constant) : std::nullopt;
}

std::optional<UniqueIRID> UniqueIRReader::getModuleID(
    const llvm::Module *module) {
  auto constant = getModuleConstID(module);
  return constant ? getID(*constant) : std::nullopt;
}

std::optional<UniqueIRID> UniqueIRReader::getLoopID(const llvm::Loop *loop) {
  auto constant = getLoopConstID(loop);
  return constant ? getID(*constant) : std::nullopt;
}

std::optional<llvm::Constant *> UniqueIRReader::getConstFromNode(
    const llvm::MDNode *node,
    unsigned operand) {
  if (!node || node->getNumOperands() <= operand) {
    return std::nullopt;
  }
  auto *constant = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      node->getOperand(operand));
  if (!constant) {
    return std::nullopt;
  }
  return constant->getValue();
}

std::optional<llvm::Constant *> UniqueIRReader::getLoopIDConstFromNode(
    const llvm::MDNode *node) {
  if (!node) {
    return std::nullopt;
  }
  for (unsigned index = 1; index < node->getNumOperands(); ++index) {
    auto *tuple = llvm::dyn_cast_or_null<llvm::MDTuple>(node->getOperand(index));
    if (!tuple || tuple->getNumOperands() < 2) {
      continue;
    }
    auto *name = llvm::dyn_cast_or_null<llvm::MDString>(tuple->getOperand(0));
    if (!name || name->getString() != UniqueIRConstants::LoopID) {
      continue;
    }
    auto *constant = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
        tuple->getOperand(1));
    if (constant) {
      return constant->getValue();
    }
  }
  return std::nullopt;
}

std::optional<UniqueIRID> UniqueIRReader::getID(const llvm::Constant *constant) {
  auto *constant_int = llvm::dyn_cast_or_null<llvm::ConstantInt>(constant);
  if (!constant_int) {
    return std::nullopt;
  }
  return constant_int->getZExtValue();
}

} // namespace lotus
