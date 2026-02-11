//===- DDAClient.h -- DDA clients (SVF-style) -----------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DDAClient: chooses which pointers to query and can hook into DDA.
// - DDAClient: all top-level pointers or user-specified queries.
// - FunptrDDAClient: function pointers at indirect call sites.
// - AliasDDAClient: load src, store dst, GEP src (for alias-style queries).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/SVFG/SVFGBase.h"

#include <vector>

namespace llvm {
class Value;
class CallBase;
class Module;
} // namespace llvm

namespace lotus {
namespace analysis {

class SVFG;
class SVFGNode;
class DemandDrivenAA;

/// Base DDA client: collects candidate pointers for demand-driven queries.
class DDAClient {
public:
  DDAClient() : svfg_(nullptr), solveAll_(true) {}
  virtual ~DDAClient() = default;

  void setSVFG(SVFG *g) { svfg_ = g; }
  SVFG *getSVFG() const { return svfg_; }
  void setModule(const llvm::Module *M) { module_ = M; }
  const llvm::Module *getModule() const { return module_; }

  /// Collect candidate pointers (Value*) to be queried. Uses getSVFG() (and getModule() for Funptr) if set.
  virtual std::vector<const llvm::Value *> &collectCandidateQueries();
  const std::vector<const llvm::Value *> &getCandidateQueries() const {
    return candidateQueries_;
  }

  /// Run DDA for each candidate (calls dda->getPointsTo for each).
  virtual void answerQueries(DemandDrivenAA *dda);

  /// Callback during backward traversal (optional).
  virtual void handleStatement(const SVFGNode *node, uint32_t curNodeId) {
    (void)node;
    (void)curNodeId;
  }

  /// Statistics after answerQueries (optional).
  virtual void performStat(DemandDrivenAA *dda) { (void)dda; }

  void setSolveAll(bool v) { solveAll_ = v; }
  bool getSolveAll() const { return solveAll_; }
  void addQuery(const llvm::Value *v) {
    userQueries_.push_back(v);
    solveAll_ = false;
  }

protected:
  void addCandidate(const llvm::Value *v);

  SVFG *svfg_ = nullptr;
  const llvm::Module *module_ = nullptr;
  std::vector<const llvm::Value *> candidateQueries_;
  std::vector<const llvm::Value *> userQueries_;
  bool solveAll_;
};

/// Client that collects only function pointers at indirect call sites.
class FunptrDDAClient : public DDAClient {
public:
  FunptrDDAClient() = default;
  std::vector<const llvm::Value *> &collectCandidateQueries() override;
  void performStat(DemandDrivenAA *dda) override;
};

/// Client that collects load pointer operands, store pointer operands, GEP base pointers.
class AliasDDAClient : public DDAClient {
public:
  AliasDDAClient() = default;
  std::vector<const llvm::Value *> &collectCandidateQueries() override;
  void performStat(DemandDrivenAA *dda) override;
};

} // namespace analysis
} // namespace lotus
