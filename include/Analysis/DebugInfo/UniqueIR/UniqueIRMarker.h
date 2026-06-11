/** @file UniqueIRMarker.h @brief Unique IR marker for identifying LLVM instructions. */
#pragma once

#include "Analysis/DebugInfo/UniqueIR/UniqueIRConstants.h"

#include <functional>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

namespace lotus {

enum class UniqueIRMarkerMode { Instrument, Reinstrument, Renumber };

class UniqueIRMarker {
public:
  using LoopInfoProvider = std::function<llvm::LoopInfo *(llvm::Function &)>;

  explicit UniqueIRMarker(
      UniqueIRMarkerMode mode = UniqueIRMarkerMode::Reinstrument);

  bool mark(llvm::Module &module, LoopInfoProvider get_loop_info = nullptr);

  static constexpr unsigned IDSize = sizeof(UniqueIRID) * 8;

private:
  UniqueIRMarkerMode mode_;
  UniqueIRID basic_block_counter_ = 0;
  UniqueIRID function_counter_ = 0;
  UniqueIRID instruction_counter_ = 0;
  UniqueIRID loop_counter_ = 0;
  UniqueIRID module_counter_ = 0;

  llvm::MDNode *buildIDNode(llvm::LLVMContext &context, UniqueIRID id) const;
  llvm::MDNode *buildLoopIDNode(llvm::Loop &loop, UniqueIRID id) const;

  bool shouldWrite(const llvm::MDNode *existing) const;
  bool shouldWriteLoop(const llvm::Loop &loop) const;

  UniqueIRID nextInstructionID();
  UniqueIRID nextModuleID();
};

} // namespace lotus
