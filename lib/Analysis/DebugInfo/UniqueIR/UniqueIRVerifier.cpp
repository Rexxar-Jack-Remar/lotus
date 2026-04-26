#include "Analysis/DebugInfo/UniqueIR/UniqueIRVerifier.h"

#include "Analysis/DebugInfo/UniqueIR/UniqueIRReader.h"

#include <llvm/IR/InstIterator.h>

namespace lotus {

bool UniqueIRVerifier::verify(const llvm::Module &module,
                              LoopInfoProvider get_loop_info) const {
  if (!UniqueIRReader::getModuleID(&module)) {
    return false;
  }

  std::set<UniqueIRID> function_ids;
  std::set<UniqueIRID> basic_block_ids;
  std::set<UniqueIRID> instruction_ids;
  std::set<UniqueIRID> loop_ids;

  for (const auto &function : module) {
    auto function_id = UniqueIRReader::getFunctionID(&function);
    if (!function_id || !function_ids.insert(*function_id).second) {
      return false;
    }

    for (const auto &basic_block : function) {
      auto basic_block_id = UniqueIRReader::getBasicBlockID(&basic_block);
      if (!basic_block_id || !basic_block_ids.insert(*basic_block_id).second) {
        return false;
      }
      for (const auto &instruction : basic_block) {
        auto instruction_id = UniqueIRReader::getInstructionID(&instruction);
        if (!instruction_id || !instruction_ids.insert(*instruction_id).second) {
          return false;
        }
      }
    }
  }

  if (!get_loop_info) {
    return true;
  }

  for (auto &function_ref : module) {
    auto &function = const_cast<llvm::Function &>(function_ref);
    if (function.empty()) {
      continue;
    }
    auto *loop_info = get_loop_info(function);
    if (!loop_info) {
      continue;
    }
    for (auto *loop : loop_info->getLoopsInPreorder()) {
      auto loop_id = UniqueIRReader::getLoopID(loop);
      if (!loop_id || !loop_ids.insert(*loop_id).second) {
        return false;
      }
    }
  }

  return true;
}

} // namespace lotus
