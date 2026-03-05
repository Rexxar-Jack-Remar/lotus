//===- GSA.h - Gated SSA construction interfaces --------------*- C++ -*-===//
//
// This file declares the public interfaces for constructing Gated
// Single-Assignment (GSA) form. The transformation augments SSA by
// introducing gating (gamma) functions that explicitly encode the control
// flow guarding each value flowing into a join point.
//
// The implementation lives in lib/IR/GSA and is intentionally independent
// from verification-specific utilities so it can be reused by general IR
// analyses and transformations.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

namespace llvm {
class BasicBlock;
class PHINode;
class Value;
class LoopInfo;
} // namespace llvm

namespace gsa {

/// Exposes block-level control dependence information.
class ControlDependenceAnalysis {
public:
  virtual ~ControlDependenceAnalysis() = default;

  /// All blocks that \p BB is control dependent on, sorted in reverse
  /// topological order.
  virtual llvm::ArrayRef<llvm::BasicBlock *>
  getCDBlocks(llvm::BasicBlock *BB) const = 0;

  /// Returns true if there is a CFG path from \p Src to \p Dst.
  virtual bool isReachable(llvm::BasicBlock *Src,
                           llvm::BasicBlock *Dst) const = 0;

  /// Returns an integer that respects the topological ordering of the CFG.
  /// A smaller value means closer to the start of the function.
  virtual unsigned getBBTopoIdx(llvm::BasicBlock *BB) const = 0;
};

/// Module pass wrapper for ControlDependenceAnalysis.
class ControlDependenceAnalysisPass : public llvm::ModulePass {
public:
  static char ID;

  ControlDependenceAnalysisPass();

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F);
  bool runOnModule(llvm::Module &M) override;

  llvm::StringRef getPassName() const override;
  void print(llvm::raw_ostream &os, const llvm::Module *M) const override;

  bool hasAnalysisFor(const llvm::Function &F) const;
  ControlDependenceAnalysis &
  getControlDependenceAnalysis(const llvm::Function &F);

private:
  llvm::DenseMap<const llvm::Function *,
                 std::unique_ptr<ControlDependenceAnalysis>>
      m_analyses;
};

llvm::ModulePass *createControlDependenceAnalysisPass();

/// Exposes the mapping between PHI nodes and their gating gamma nodes.
class GateAnalysis {
public:
  virtual ~GateAnalysis() = default;

  /// Returns the gamma value guarding \p PN. This may be a SelectInst or
  /// another value depending on the control flow.
  virtual llvm::Value *getGamma(llvm::PHINode *PN) const = 0;

  /// Returns true if \p PN corresponds to a Mu node (loop header PHI).
  virtual bool isMu(llvm::PHINode *PN) const = 0;

  /// Returns true if \p PN corresponds to an Eta node (loop exit PHI).
  virtual bool isEta(llvm::PHINode *PN) const = 0;

  /// True if thinned gating was requested (i.e., gamma nodes may omit undef
  /// operands), false otherwise.
  virtual bool isThinned() const = 0;
};

/// Module pass that builds the Gated SSA form for all functions.
class GateAnalysisPass : public llvm::ModulePass {
public:
  static char ID;

  GateAnalysisPass();

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F, ControlDependenceAnalysis &CDA,
                     llvm::LoopInfo &LI);
  bool runOnModule(llvm::Module &M) override;

  llvm::StringRef getPassName() const override;
  void print(llvm::raw_ostream &os, const llvm::Module *M) const override;

  bool hasAnalysisFor(const llvm::Function &F) const;
  GateAnalysis &getGateAnalysis(const llvm::Function &F);

private:
  llvm::DenseMap<const llvm::Function *, std::unique_ptr<GateAnalysis>>
      m_analyses;
};

llvm::ModulePass *createGateAnalysisPass();

} // namespace gsa
