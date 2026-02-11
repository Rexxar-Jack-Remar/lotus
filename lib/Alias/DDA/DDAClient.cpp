//===- DDAClient.cpp -- DDA clients (SVF-style) ---------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/DDAClient.h"
#include "Alias/DDA/DemandDrivenAA.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <unordered_set>

using namespace lotus::analysis;
using namespace llvm;

void DDAClient::addCandidate(const llvm::Value *v) {
  if (!v || !v->getType()->isPointerTy())
    return;
  candidateQueries_.push_back(v);
}

std::vector<const llvm::Value *> &DDAClient::collectCandidateQueries() {
  candidateQueries_.clear();
  if (!svfg_)
    return candidateQueries_;
  if (!solveAll_) {
    for (const llvm::Value *v : userQueries_)
      addCandidate(v);
    return candidateQueries_;
  }
  for (auto it = svfg_->begin(), e = svfg_->end(); it != e; ++it) {
    SVFGNode *node = it->second;
    if (!node)
      continue;
    const llvm::Value *v = node->getValue();
    if (!v || !v->getType()->isPointerTy())
      continue;
    if (node->getNodeKind() == SVFGK::Addr || node->getNodeKind() == SVFGK::Copy ||
        node->getNodeKind() == SVFGK::Phi || node->getNodeKind() == SVFGK::IntraPhi ||
        node->getNodeKind() == SVFGK::FormalParm ||
        node->getNodeKind() == SVFGK::ActualParm ||
        node->getNodeKind() == SVFGK::FormalRet ||
        node->getNodeKind() == SVFGK::ActualRet)
      addCandidate(v);
  }
  return candidateQueries_;
}

void DDAClient::answerQueries(DemandDrivenAA *dda) {
  if (!dda || !dda->getSVFG())
    return;
  setSVFG(dda->getSVFG());
  collectCandidateQueries();
  for (const llvm::Value *ptr : candidateQueries_)
    (void)dda->getPointsTo(ptr);
  performStat(dda);
}

std::vector<const llvm::Value *> &FunptrDDAClient::collectCandidateQueries() {
  candidateQueries_.clear();
  if (!solveAll_ && !userQueries_.empty()) {
    for (const llvm::Value *v : userQueries_)
      addCandidate(v);
    return candidateQueries_;
  }
  if (!module_)
    return candidateQueries_;
  for (const Function &F : *module_) {
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const CallBase *cb = llvm::dyn_cast<CallBase>(&I);
        if (!cb || cb->getCalledFunction())
          continue;
        const llvm::Value *called = cb->getCalledOperand();
        if (called && called->getType()->isPointerTy())
          addCandidate(called);
      }
    }
  }
  return candidateQueries_;
}

void FunptrDDAClient::performStat(DemandDrivenAA *dda) {
  (void)dda;
  // Optional: compare with baseline PTA, count resolved targets, etc.
}

std::vector<const llvm::Value *> &AliasDDAClient::collectCandidateQueries() {
  candidateQueries_.clear();
  if (!solveAll_ && !userQueries_.empty()) {
    for (const llvm::Value *v : userQueries_)
      addCandidate(v);
    return candidateQueries_;
  }
  if (!svfg_)
    return candidateQueries_;
  std::unordered_set<const llvm::Value *> seen;
  for (auto it = svfg_->begin(), e = svfg_->end(); it != e; ++it) {
    SVFGNode *node = it->second;
    if (!node)
      continue;
    const llvm::Value *ptr = nullptr;
    if (node->getNodeKind() == SVFGK::Load) {
      const LoadSVFGNode *load = llvm::cast<LoadSVFGNode>(node);
      if (llvm::isa_and_nonnull<LoadInst>(load->getValue()))
        ptr = llvm::cast<LoadInst>(load->getValue())->getPointerOperand();
    } else if (node->getNodeKind() == SVFGK::Store) {
      const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(node);
      if (llvm::isa_and_nonnull<StoreInst>(store->getValue()))
        ptr = llvm::cast<StoreInst>(store->getValue())->getPointerOperand();
    } else if (node->getNodeKind() == SVFGK::Gep) {
      const GepSVFGNode *gep = llvm::cast<GepSVFGNode>(node);
      if (llvm::isa_and_nonnull<GetElementPtrInst>(gep->getValue()))
        ptr = llvm::cast<GetElementPtrInst>(gep->getValue())->getPointerOperand();
    }
    if (ptr && ptr->getType()->isPointerTy() && seen.insert(ptr).second)
      addCandidate(ptr);
  }
  return candidateQueries_;
}

void AliasDDAClient::performStat(DemandDrivenAA *dda) {
  (void)dda;
  // Optional: run alias queries between load src / store dst pairs, etc.
}
