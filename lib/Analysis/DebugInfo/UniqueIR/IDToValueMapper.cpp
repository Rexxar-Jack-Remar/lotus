#include "Analysis/DebugInfo/UniqueIR/IDToValueMapper.h"

#include "Analysis/DebugInfo/UniqueIR/UniqueIRReader.h"

namespace lotus {

IDToInstructionMapper::IDToInstructionMapper(llvm::Module &module)
    : module_(module) {}

std::unique_ptr<std::map<UniqueIRID, llvm::Instruction *>>
IDToInstructionMapper::idToValueMap(const std::set<UniqueIRID> &ids) const {
  auto mapping = std::make_unique<std::map<UniqueIRID, llvm::Instruction *>>();
  for (auto &function : module_) {
    for (auto &basic_block : function) {
      for (auto &instruction : basic_block) {
        auto id = UniqueIRReader::getInstructionID(&instruction);
        if (id && ids.count(*id) > 0) {
          mapping->insert({*id, &instruction});
        }
      }
    }
  }
  return mapping;
}

IDToFunctionMapper::IDToFunctionMapper(llvm::Module &module) : module_(module) {}

std::unique_ptr<std::map<UniqueIRID, llvm::Function *>>
IDToFunctionMapper::idToValueMap(const std::set<UniqueIRID> &ids) const {
  auto mapping = std::make_unique<std::map<UniqueIRID, llvm::Function *>>();
  for (auto &function : module_) {
    auto id = UniqueIRReader::getFunctionID(&function);
    if (id && ids.count(*id) > 0) {
      mapping->insert({*id, &function});
    }
  }
  return mapping;
}

} // namespace lotus
