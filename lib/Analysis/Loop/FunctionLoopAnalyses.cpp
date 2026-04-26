/*
 * Copyright 2026  Lotus contributors
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
#include "Analysis/Loop/FunctionLoopAnalyses.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"

namespace lotus {
namespace analysis {
namespace loop {

FunctionLoopAnalyses::FunctionLoopAnalyses(Function &F,
                                           LoopInfo &LI,
                                           DominatorTree &DT,
                                           PostDominatorTree &PDT)
    : function{&F}, dominatorTree{&DT}, postDominatorTree{&PDT} {
  std::vector<LoopStructure *> loopPtrs;
  loopPtrs.reserve(LI.getLoopsInPreorder().size());

  for (auto *loop : LI.getLoopsInPreorder()) {
    auto owned = std::unique_ptr<LoopStructure>(new LoopStructure(loop));
    auto *raw = owned.get();
    this->loopByHeader[raw->getHeader()] = raw;
    this->ownedLoopStructures.push_back(std::move(owned));
    loopPtrs.push_back(raw);
  }

  if (loopPtrs.empty()) {
    return;
  }

  noelle::DominatorSummary summary(DT, PDT);
  std::unordered_map<Function *, noelle::DominatorSummary *> doms;
  doms[&F] = &summary;
  this->forest.reset(new LoopForest(loopPtrs, doms));

  for (auto *loopStructure : loopPtrs) {
    auto *loopNode = this->forest->getNode(loopStructure);
    if (!loopNode) {
      continue;
    }
    auto ownedContent = std::unique_ptr<LoopContent>(new LoopContent(loopNode));
    auto *rawContent = ownedContent.get();
    this->contentByHeader[loopStructure->getHeader()] = rawContent;
    this->loopContents.push_back(std::move(ownedContent));
  }
}

Function *FunctionLoopAnalyses::getFunction(void) const { return this->function; }

LoopForest *FunctionLoopAnalyses::getLoopForest(void) const {
  return this->forest.get();
}

std::vector<LoopStructure *> FunctionLoopAnalyses::getLoopStructures(void) const {
  std::vector<LoopStructure *> loops;
  loops.reserve(this->ownedLoopStructures.size());
  for (auto const &owned : this->ownedLoopStructures) {
    loops.push_back(owned.get());
  }
  return loops;
}

std::vector<LoopContent *> FunctionLoopAnalyses::getLoopContents(void) const {
  std::vector<LoopContent *> contents;
  contents.reserve(this->loopContents.size());
  for (auto const &owned : this->loopContents) {
    contents.push_back(owned.get());
  }
  return contents;
}

void FunctionLoopAnalyses::materializeDependenceGraphs(pdg::ProgramGraph &pdg) {
  for (auto const &ownedContent : this->loopContents) {
    ownedContent->materializeDependenceGraph(pdg);
  }
}

void FunctionLoopAnalyses::materializeScalarAnalyses(
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    LoopLDGBuilderOptions options) {
  assert(this->dominatorTree != nullptr);
  assert(this->postDominatorTree != nullptr);
  noelle::DominatorSummary summary(*this->dominatorTree, *this->postDominatorTree);
  for (auto const &ownedContent : this->loopContents) {
    ownedContent->materializeScalarAnalyses(SE, LI, summary, options);
    auto *llvmLoop = LI.getLoopFor(ownedContent->getLoopStructure()->getHeader());
    if (llvmLoop != nullptr) {
      ownedContent->setCompileTimeTripCount(SE.getSmallConstantTripCount(llvmLoop));
    }
  }
}

void FunctionLoopAnalyses::materializeLoopEnvironments(void) {
  for (auto const &ownedContent : this->loopContents) {
    ownedContent->materializeEnvironment();
  }
}

void FunctionLoopAnalyses::materializeLoopCarriedDependencies(
    llvm::DominatorTree &DT,
    llvm::PostDominatorTree &PDT) {
  noelle::DominatorSummary summary(DT, PDT);
  for (auto const &ownedContent : this->loopContents) {
    ownedContent->materializeLoopCarriedDependencies(summary);
  }
}

void FunctionLoopAnalyses::materializeIterationSpaceAnalyses(
    llvm::ScalarEvolution &SE) {
  for (auto const &ownedContent : this->loopContents) {
    ownedContent->materializeIterationSpaceAnalysis(SE);
  }
}

void FunctionLoopAnalyses::materializeSCCAttrs(llvm::DominatorTree &DT,
                                               llvm::PostDominatorTree &PDT,
                                               bool enableFloatAsReal) {
  noelle::DominatorSummary summary(DT, PDT);
  for (auto const &ownedContent : this->loopContents) {
    ownedContent->materializeSCCAttrs(enableFloatAsReal, summary);
  }
}

LoopContent *FunctionLoopAnalyses::getLoopContent(Loop &loop) const {
  return this->getLoopContent(loop.getHeader());
}

LoopContent *FunctionLoopAnalyses::getLoopContent(BasicBlock *header) const {
  auto it = this->contentByHeader.find(header);
  if (it == this->contentByHeader.end()) {
    return nullptr;
  }
  return it->second;
}

llvm::AnalysisKey FunctionLoopAnalysesPass::Key;

FunctionLoopAnalysesPass::Result
FunctionLoopAnalysesPass::run(Function &F, FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
  return FunctionLoopAnalyses(F, LI, DT, PDT);
}

char FunctionLoopAnalysesWrapperPass::ID = 0;

FunctionLoopAnalysesWrapperPass::FunctionLoopAnalysesWrapperPass()
    : FunctionPass(ID) {}

bool FunctionLoopAnalysesWrapperPass::runOnFunction(Function &F) {
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &PDT = getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
  this->result.reset(new FunctionLoopAnalyses(F, LI, DT, PDT));
  return false;
}

void FunctionLoopAnalysesWrapperPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<PostDominatorTreeWrapperPass>();
}

FunctionLoopAnalyses &FunctionLoopAnalysesWrapperPass::getResult(void) {
  assert(this->result != nullptr);
  return *this->result;
}

const FunctionLoopAnalyses &
FunctionLoopAnalysesWrapperPass::getResult(void) const {
  assert(this->result != nullptr);
  return *this->result;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
