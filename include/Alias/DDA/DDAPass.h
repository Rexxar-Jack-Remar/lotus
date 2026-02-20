//===- DDAPass.h -- DDA driver (SVF-style) -------------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DDAPass: selects DDA mode (FlowDDA / ContextDDA) and client
// (DDAClient / FunptrDDAClient / AliasDDAClient), runs analysis, answerQueries.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/DDAClient.h"
#include "Alias/DDA/FlowDDA.h"

#include <memory>
#include <string>

namespace llvm {
class Module;
class Value;
} // namespace llvm

namespace lotus {
namespace analysis {

class ContextDDA;

enum class DDAKind {
  FlowS_DDA,  /// Flow-sensitive, context-insensitive (FlowDDA)
  Cxt_DDA     /// Flow-sensitive, context-sensitive (ContextDDA)
};

enum class DDAClientKind {
  All,     /// All top-level pointers (DDAClient, solveAll)
  Funptr,  /// Function pointers at indirect call sites
  Alias    /// Load src, store dst, GEP src (AliasDDAClient)
};

/// Demand-driven analysis driver: mode + client, run + answerQueries.
class DDAPass {
public:
  DDAPass() = default;
  ~DDAPass();

  void runOnModule(llvm::Module &M);
  void selectClient(DDAClientKind k);
  void setClient(std::unique_ptr<DDAClient> client);
  DDAClient *getClient() const { return client_.get(); }
  /// Add one explicit query pointer (switches client to user-query mode).
  void addQuery(const llvm::Value *v);

  void setDDAKind(DDAKind k) { kind_ = k; }
  DDAKind getDDAKind() const { return kind_; }

  FlowDDA *getFlowDDA() const { return flowDDA_.get(); }
  ContextDDA *getContextDDA() const { return contextDDA_.get(); }
  /// Convenience alias query over the current FlowDDA results.
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2) const;

private:
  void runPointerAnalysis(llvm::Module &M, DDAKind k);

  DDAKind kind_ = DDAKind::FlowS_DDA;
  std::unique_ptr<DDAClient> client_;
  std::unique_ptr<FlowDDA> flowDDA_;
  std::unique_ptr<ContextDDA> contextDDA_;
};

} // namespace analysis
} // namespace lotus
