#include "Analysis/DebugInfo/UniqueIR/UniqueIRMarker.h"

#include "Analysis/DebugInfo/UniqueIR/UniqueIRReader.h"

#include <cassert>
#include <limits>

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>

namespace lotus {

UniqueIRMarker::UniqueIRMarker(UniqueIRMarkerMode mode) : mode_(mode) {}

bool UniqueIRMarker::mark(llvm::Module &module, LoopInfoProvider get_loop_info) {
  bool changed = false;

  auto *module_meta = module.getOrInsertNamedMetadata(
      UniqueIRConstants::ModuleID);
  if (mode_ != UniqueIRMarkerMode::Renumber || module_meta->getNumOperands() == 1) {
    module_meta->clearOperands();
    module_meta->addOperand(buildIDNode(module.getContext(), nextModuleID()));
    changed = true;
  }

  for (auto &function : module) {
    auto *function_meta = function.getMetadata(UniqueIRConstants::FunctionID);
    if (shouldWrite(function_meta)) {
      function.setMetadata(
          UniqueIRConstants::FunctionID,
          buildIDNode(function.getContext(), function_counter_++));
      changed = true;
    }

    if (get_loop_info && !function.empty()) {
      if (auto *loop_info = get_loop_info(function)) {
        for (auto *loop : loop_info->getLoopsInPreorder()) {
          if (loop && shouldWriteLoop(*loop)) {
            loop->setLoopID(buildLoopIDNode(*loop, loop_counter_++));
            changed = true;
          }
        }
      }
    }

    for (auto &basic_block : function) {
      if (!basic_block.empty()) {
        auto *bb_meta = basic_block.front().getMetadata(
            UniqueIRConstants::BasicBlockID);
        if (shouldWrite(bb_meta)) {
          basic_block.front().setMetadata(
              UniqueIRConstants::BasicBlockID,
              buildIDNode(basic_block.getContext(), basic_block_counter_++));
          changed = true;
        }
      }

      for (auto &instruction : basic_block) {
        auto *instruction_meta = instruction.getMetadata(
            UniqueIRConstants::InstructionID);
        if (shouldWrite(instruction_meta)) {
          instruction.setMetadata(UniqueIRConstants::InstructionID,
                                  buildIDNode(instruction.getContext(),
                                              nextInstructionID()));
          changed = true;
        }
      }
    }
  }

  return changed;
}

llvm::MDNode *UniqueIRMarker::buildIDNode(llvm::LLVMContext &context,
                                          UniqueIRID id) const {
  return llvm::MDNode::get(
      context,
      llvm::ConstantAsMetadata::get(
          llvm::ConstantInt::get(context, llvm::APInt(IDSize, id, false))));
}

llvm::MDNode *UniqueIRMarker::buildLoopIDNode(llvm::Loop &loop,
                                             UniqueIRID id) const {
  auto &context = loop.getHeader()->getContext();
  llvm::SmallVector<llvm::Metadata *, 8> operands;
  operands.push_back(nullptr);

  if (auto *old_id = loop.getLoopID()) {
    for (unsigned index = 1; index < old_id->getNumOperands(); ++index) {
      auto *tuple = llvm::dyn_cast_or_null<llvm::MDTuple>(
          old_id->getOperand(index));
      auto *name = tuple && tuple->getNumOperands() > 0
                       ? llvm::dyn_cast_or_null<llvm::MDString>(
                             tuple->getOperand(0))
                       : nullptr;
      if (name && name->getString() == UniqueIRConstants::LoopID) {
        continue;
      }
      operands.push_back(old_id->getOperand(index));
    }
  }

  llvm::Metadata *id_operands[] = {
      llvm::MDString::get(context, UniqueIRConstants::LoopID),
      llvm::ConstantAsMetadata::get(
          llvm::ConstantInt::get(context, llvm::APInt(IDSize, id, false)))};
  operands.push_back(llvm::MDNode::get(context, id_operands));

  auto *node = llvm::MDNode::get(context, operands);
  node->replaceOperandWith(0, node);
  return node;
}

bool UniqueIRMarker::shouldWrite(const llvm::MDNode *existing) const {
  switch (mode_) {
  case UniqueIRMarkerMode::Instrument:
    return existing == nullptr;
  case UniqueIRMarkerMode::Reinstrument:
    return true;
  case UniqueIRMarkerMode::Renumber:
    return existing != nullptr;
  }
  return false;
}

bool UniqueIRMarker::shouldWriteLoop(const llvm::Loop &loop) const {
  const bool has_id = UniqueIRReader::getLoopID(&loop).has_value();
  switch (mode_) {
  case UniqueIRMarkerMode::Instrument:
    return !has_id;
  case UniqueIRMarkerMode::Reinstrument:
    return true;
  case UniqueIRMarkerMode::Renumber:
    return has_id;
  }
  return false;
}

UniqueIRID UniqueIRMarker::nextInstructionID() {
  assert(instruction_counter_ <= std::numeric_limits<UniqueIRID>::max() - 1 &&
         "Unique IR instruction counter overflow");
  return instruction_counter_++;
}

UniqueIRID UniqueIRMarker::nextModuleID() {
  assert(module_counter_ <= std::numeric_limits<UniqueIRID>::max() - 1 &&
         "Unique IR module counter overflow");
  return module_counter_++;
}

} // namespace lotus
