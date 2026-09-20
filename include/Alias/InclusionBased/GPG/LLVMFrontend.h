#pragma once

#include "Alias/InclusionBased/GPG/Config.h"
#include "Alias/InclusionBased/GPG/Graph.h"
#include "Alias/InclusionBased/GPG/ProgramModel.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace llvm {
class Constant;
class Function;
class Instruction;
class User;
class Value;
} // namespace llvm

namespace lotus::gpg {

class LLVMFrontend {
public:
  LLVMFrontend(ProgramModel &model, const GPGConfig &config);

  GPG buildInitialGPG(llvm::Function &function);
  const GPUSet &globalInitializers() const { return global_initializers_; }

  std::vector<Access> readAccesses(const llvm::Value *value);

private:
  ProgramModel &model_;
  const GPGConfig &config_;
  GPUSet global_initializers_;
  std::uint64_t next_gpu_provenance_ = 1;

  std::optional<GPB> translateInstruction(llvm::Instruction &instruction,
                                          GPBId id);
  std::vector<Indirection> gepPath(const llvm::User &gep,
                                   bool &pointer_arithmetic) const;
  Access append(const Access &base,
                const std::vector<Indirection> &suffix) const;
  Access appendDereference(const Access &base) const;
  GPU makeGPU(const Access &source, const Access &target,
              const llvm::Instruction &instruction,
              GPUKind kind = GPUKind::Assignment);
  GPU makeUseGPU(const Access &target, const llvm::Instruction &instruction);
  void addPointerOperandUses(GPB &block, llvm::Instruction &instruction);
  void addBoundaryDefinitions(GPG &graph, const llvm::Function &function) const;

  void collectGlobalInitializers();
  void emitInitializer(const llvm::Constant &constant, const Access &base,
                       std::vector<Indirection> path);
  void emitZeroInitializer(const llvm::Type &type, const Access &base,
                           std::vector<Indirection> path);
};

} // namespace lotus::gpg
