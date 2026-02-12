//===- DDAPass.cpp -- DDA driver (SVF-style) ------------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/DDAPass.h"
#include "Alias/DDA/ContextDDA.h"

#include <llvm/IR/Module.h>

using namespace lotus::analysis;
using namespace llvm;

DDAPass::~DDAPass() = default;

void DDAPass::selectClient(DDAClientKind k) {
  switch (k) {
  case DDAClientKind::All:
    client_ = std::make_unique<DDAClient>();
    client_->setSolveAll(true);
    break;
  case DDAClientKind::Funptr:
    client_ = std::make_unique<FunptrDDAClient>();
    break;
  case DDAClientKind::Alias:
    client_ = std::make_unique<AliasDDAClient>();
    break;
  }
}

void DDAPass::setClient(std::unique_ptr<DDAClient> client) {
  client_ = std::move(client);
}

void DDAPass::runOnModule(Module &M) {
  if (!client_)
    return;
  runPointerAnalysis(M, kind_);
}

void DDAPass::runPointerAnalysis(Module &M, DDAKind k) {
  flowDDA_ = std::make_unique<FlowDDA>();
  if (!flowDDA_->run(M))
    return;
  flowDDA_->setClient(client_.get());
  switch (k) {
  case DDAKind::FlowS_DDA:
    flowDDA_->answerQueries();
    break;
  case DDAKind::Cxt_DDA:
    contextDDA_ = std::make_unique<ContextDDA>(flowDDA_.get(), client_.get());
    contextDDA_->run(M);
    contextDDA_->initInsensitiveEdges();
    contextDDA_->answerQueries();
    break;
  }
}
