/**
 * @file LinuxKernelLifetimeAnalysis.h
 * @brief Kernel allocation and asynchronous owner-lifetime analysis.
 */

#pragma once

#include <utility>
#include <vector>

namespace llvm {
class Instruction;
} // namespace llvm

namespace kernel {

class LinuxKernelExecutionGraph;
class LinuxKernelProcessModel;

class LinuxKernelLifetimeAnalysis {
public:
  enum class HazardKind { DIRECT_USE_AFTER_FREE, ASYNC_CALLBACK_AFTER_FREE };

  struct Hazard {
    HazardKind kind = HazardKind::DIRECT_USE_AFTER_FREE;
    const llvm::Instruction *free_inst = nullptr;
    const llvm::Instruction *use_or_submit_inst = nullptr;
  };

  LinuxKernelLifetimeAnalysis(const LinuxKernelProcessModel &process_model,
                              const LinuxKernelExecutionGraph &execution_graph)
      : process_model_(process_model), execution_graph_(execution_graph) {}

  void analyze();

  const std::vector<Hazard> &getHazards() const { return hazards_; }
  std::vector<const llvm::Instruction *> findUseAfterFree() const;
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findAsyncLifetimeHazards() const;

private:
  const LinuxKernelProcessModel &process_model_;
  const LinuxKernelExecutionGraph &execution_graph_;
  std::vector<Hazard> hazards_;
};

} // namespace kernel
