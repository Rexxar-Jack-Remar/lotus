#pragma once

#include "Analysis/DebugInfo/UniqueIR/UniqueIRConstants.h"

#include <map>
#include <memory>
#include <set>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace lotus {

class IDToInstructionMapper {
public:
  explicit IDToInstructionMapper(llvm::Module &module);

  std::unique_ptr<std::map<UniqueIRID, llvm::Instruction *>> idToValueMap(
      const std::set<UniqueIRID> &ids) const;

private:
  llvm::Module &module_;
};

class IDToFunctionMapper {
public:
  explicit IDToFunctionMapper(llvm::Module &module);

  std::unique_ptr<std::map<UniqueIRID, llvm::Function *>> idToValueMap(
      const std::set<UniqueIRID> &ids) const;

private:
  llvm::Module &module_;
};

} // namespace lotus
