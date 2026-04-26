/*
 * Copyright 2026 Lotus contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef LOTUS_ANALYSIS_LOOP_FUNCTIONLOOPANALYSES_H
#define LOTUS_ANALYSIS_LOOP_FUNCTIONLOOPANALYSES_H

#include "Analysis/Loop/LoopContent.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/PassManager.h"

#include <memory>

namespace lotus {
namespace analysis {
namespace loop {

class FunctionLoopAnalyses {
public:
  FunctionLoopAnalyses() = default;
  FunctionLoopAnalyses(Function &F,
                       LoopInfo &LI,
                       DominatorTree &DT,
                       PostDominatorTree &PDT);

  FunctionLoopAnalyses(FunctionLoopAnalyses &&) = default;
  FunctionLoopAnalyses &operator=(FunctionLoopAnalyses &&) = default;

  FunctionLoopAnalyses(const FunctionLoopAnalyses &) = delete;
  FunctionLoopAnalyses &operator=(const FunctionLoopAnalyses &) = delete;

  Function *getFunction(void) const;

  LoopForest *getLoopForest(void) const;

  std::vector<LoopStructure *> getLoopStructures(void) const;
  std::vector<LoopContent *> getLoopContents(void) const;

  void materializeDependenceGraphs(pdg::ProgramGraph &pdg);
  void materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                 llvm::LoopInfo &LI,
                                 LoopLDGBuilderOptions options = {});
  void materializeLoopEnvironments(void);
  void materializeLoopCarriedDependencies(llvm::DominatorTree &DT,
                                          llvm::PostDominatorTree &PDT);
  void materializeIterationSpaceAnalyses(llvm::ScalarEvolution &SE);
  void materializeSCCAttrs(llvm::DominatorTree &DT,
                           llvm::PostDominatorTree &PDT,
                           bool enableFloatAsReal = false);

  LoopContent *getLoopContent(Loop &loop) const;
  LoopContent *getLoopContent(BasicBlock *header) const;

private:
  Function *function{nullptr};
  DominatorTree *dominatorTree{nullptr};
  PostDominatorTree *postDominatorTree{nullptr};
  std::vector<std::unique_ptr<LoopStructure>> ownedLoopStructures;
  std::unordered_map<BasicBlock *, LoopStructure *> loopByHeader;
  std::unique_ptr<LoopForest> forest;
  std::vector<std::unique_ptr<LoopContent>> loopContents;
  std::unordered_map<BasicBlock *, LoopContent *> contentByHeader;
};

class FunctionLoopAnalysesPass
    : public llvm::AnalysisInfoMixin<FunctionLoopAnalysesPass> {
public:
  using Result = FunctionLoopAnalyses;

  Result run(Function &F, FunctionAnalysisManager &FAM);

private:
  friend llvm::AnalysisInfoMixin<FunctionLoopAnalysesPass>;
  static llvm::AnalysisKey Key;
};

class FunctionLoopAnalysesWrapperPass : public FunctionPass {
public:
  static char ID;

  FunctionLoopAnalysesWrapperPass();

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  FunctionLoopAnalyses &getResult(void);
  const FunctionLoopAnalyses &getResult(void) const;

private:
  std::unique_ptr<FunctionLoopAnalyses> result;
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
