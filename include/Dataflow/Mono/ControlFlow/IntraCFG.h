#pragma once

#include "Dataflow/Mono/FlowDirection.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include <utility>
#include <vector>

namespace mono {

/// Instruction-level intraprocedural CFG interface (Phasar-like).
class IntraCFG {
public:
  using n_t = llvm::Instruction *;
  using f_t = llvm::Function *;

  virtual ~IntraCFG() = default;

  virtual std::vector<n_t> getSuccsOf(n_t Inst, FlowDirection Dir) const = 0;
  virtual std::vector<n_t> getPredsOf(n_t Inst, FlowDirection Dir) const = 0;

  /// Whether Dst is a branch target (i.e., has >1 predecessors) for the given
  /// direction. Src is provided for parity with Phasar's API.
  virtual bool isBranchTarget(n_t /*Src*/, n_t Dst, FlowDirection Dir) const {
    return getPredsOf(Dst, Dir).size() > 1;
  }

  virtual std::vector<n_t> getAllInstructionsOf(f_t Function) const = 0;
  virtual std::vector<std::pair<n_t, n_t>>
  getAllControlFlowEdges(f_t Function, FlowDirection Dir) const = 0;
};

/// Default LLVM-backed intraprocedural instruction CFG.
class LLVMIntraCFG final : public IntraCFG {
public:
  std::vector<n_t> getSuccsOf(n_t Inst, FlowDirection Dir) const override;
  std::vector<n_t> getPredsOf(n_t Inst, FlowDirection Dir) const override;

  std::vector<n_t> getAllInstructionsOf(f_t Function) const override;
  std::vector<std::pair<n_t, n_t>>
  getAllControlFlowEdges(f_t Function, FlowDirection Dir) const override;

private:
  static std::vector<n_t> getForwardSuccs(n_t Inst);
  static std::vector<n_t> getBackwardSuccs(n_t Inst);
};

} // namespace mono

// ---- Header-only implementation ----

namespace mono {

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getForwardSuccs(n_t Inst) {
  std::vector<n_t> Succs;
  if (Inst == nullptr) {
    return Succs;
  }
  if (Inst->isTerminator()) {
    for (auto *SuccBB : llvm::successors(Inst->getParent())) {
      Succs.push_back(&*SuccBB->begin());
    }
    return Succs;
  }
  if (auto *Next = Inst->getNextNode()) {
    Succs.push_back(Next);
  }
  return Succs;
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getBackwardSuccs(n_t Inst) {
  std::vector<n_t> Preds;
  if (Inst == nullptr) {
    return Preds;
  }
  auto *BB = Inst->getParent();
  if (Inst != &*BB->begin()) {
    Preds.push_back(Inst->getPrevNode());
    return Preds;
  }
  for (auto *PredBB : llvm::predecessors(BB)) {
    Preds.push_back(PredBB->getTerminator());
  }
  return Preds;
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getSuccsOf(n_t Inst, FlowDirection Dir) const {
  return Dir == FlowDirection::Forward ? getForwardSuccs(Inst)
                                       : getBackwardSuccs(Inst);
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getPredsOf(n_t Inst, FlowDirection Dir) const {
  return Dir == FlowDirection::Forward ? getBackwardSuccs(Inst)
                                       : getForwardSuccs(Inst);
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getAllInstructionsOf(f_t Function) const {
  std::vector<n_t> Insts;
  if (Function == nullptr || Function->isDeclaration()) {
    return Insts;
  }
  for (auto &BB : *Function) {
    for (auto &I : BB) {
      Insts.push_back(&I);
    }
  }
  return Insts;
}

inline std::vector<std::pair<LLVMIntraCFG::n_t, LLVMIntraCFG::n_t>>
LLVMIntraCFG::getAllControlFlowEdges(f_t Function, FlowDirection Dir) const {
  std::vector<std::pair<n_t, n_t>> Edges;
  if (Function == nullptr || Function->isDeclaration()) {
    return Edges;
  }
  for (auto &BB : *Function) {
    for (auto &I : BB) {
      for (auto *Succ : getSuccsOf(&I, Dir)) {
        Edges.push_back({&I, Succ});
      }
    }
  }
  return Edges;
}

} // namespace mono
