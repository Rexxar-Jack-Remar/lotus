#pragma once

#include "Alias/InclusionBased/GPG/Config.h"
#include "Alias/InclusionBased/GPG/Graph.h"
#include "Alias/InclusionBased/GPG/LLVMFrontend.h"
#include "Alias/InclusionBased/GPG/ProgramModel.h"
#include "Alias/InclusionBased/GPG/Result.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

#include <llvm/Pass.h>

namespace llvm {
class CallBase;
class Function;
class Module;
} // namespace llvm

namespace lotus::gpg {

class GPGAnalysisEngine {
public:
  GPGAnalysisEngine(llvm::Module &module, GPGConfig config = {});

  void run();

  const GPGResult &result() const { return result_; }
  const GPG *summary(const llvm::Function *function) const;
  const GPG *initialGPG(const llvm::Function *function) const;
  const ProgramModel &programModel() const { return model_; }

private:
  using TargetSet = std::set<const llvm::Function *>;
  using TargetMap = std::map<const llvm::CallBase *, TargetSet>;
  using FunctionSet = std::set<const llvm::Function *>;
  using SCC = std::vector<const llvm::Function *>;
  using SemanticSummary = std::pair<GPUSet, GPUSet>;

  llvm::Module &module_;
  GPGConfig config_;
  ProgramModel model_;
  LLVMFrontend frontend_;
  std::map<const llvm::Function *, GPG> initial_graphs_;
  std::map<const llvm::Function *, GPG> summaries_;
  TargetMap indirect_targets_;
  GPUSet observed_gpus_;
  GPGResult result_;
  AnalysisStats stats_;

  void buildInitialGraphs();
  bool constructSummaries();
  void constructFlowInsensitiveSummaries(bool context_sensitive);
  GPUSet solveFlowInsensitive(const GPUSet &input) const;
  GPUSet graphGPUs(const GPG &graph) const;
  GPG makeFlowInsensitiveSummary(GPUSet gpus) const;
  GPUSet instantiateFlowInsensitiveCall(const llvm::CallBase &call,
                                        const llvm::Function &callee,
                                        const GPUSet &callee_summary);
  void
  constructSCC(const SCC &scc,
               const std::map<const llvm::Function *, FunctionSet> &callees,
               const std::map<const llvm::Function *, FunctionSet> &callers);
  GPG instantiate(const llvm::Function &function,
                  const FunctionSet &current_scc, bool recursive_refinement);
  CallExpansion makeExpansion(const llvm::CallBase &call,
                              const llvm::Function &callee,
                              const GPG &callee_summary);
  GPG bottomGPG() const;
  void optimize(GPG &graph);
  SemanticSummary semantics(const GPG &graph) const;

  std::map<const llvm::Function *, FunctionSet> buildCallees() const;
  std::map<const llvm::Function *, FunctionSet> buildCallers(
      const std::map<const llvm::Function *, FunctionSet> &callees) const;
  std::vector<SCC> computeSCCs(
      const std::map<const llvm::Function *, FunctionSet> &callees) const;

  bool discoverIndirectTargets();
  bool addFallbackIndirectTargets();
  bool signaturesCompatible(const llvm::CallBase &call,
                            const llvm::Function &target) const;
  void collectResult();
};

class GPGAnalysisPass : public llvm::ModulePass {
public:
  static char ID;

  explicit GPGAnalysisPass(GPGConfig config = {});
  bool runOnModule(llvm::Module &module) override;
  void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;

  const GPGResult &getResult() const;
  const GPGAnalysisEngine *getEngine() const { return engine_.get(); }

private:
  GPGConfig config_;
  std::unique_ptr<GPGAnalysisEngine> engine_;
};

} // namespace lotus::gpg
