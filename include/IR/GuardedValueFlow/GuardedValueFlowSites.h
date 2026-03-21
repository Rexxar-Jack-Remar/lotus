#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

#include <algorithm>
#include <map>
#include <vector>

namespace llvm {
namespace gvg {

class GuardedValueFlowGraph;
class GuardedValueFlowNode;

class GuardedValueFlowSite {
public:
  enum class Kind {
    CallSite,
    ReturnSite,
    DereferenceSite,
    GEP,
    Compare,
    Alloc,
    Unknown,
  };

  GuardedValueFlowSite(Kind kind, GuardedValueFlowGraph *graph,
                       Instruction *inst)
      : kind_(kind), graph_(graph), inst_(inst) {}
  virtual ~GuardedValueFlowSite() = default;

  Kind getKind() const { return kind_; }
  GuardedValueFlowGraph *getGraph() const { return graph_; }
  Instruction *getInstruction() const { return inst_; }

private:
  Kind kind_;
  GuardedValueFlowGraph *graph_;
  Instruction *inst_;
};

class GuardedValueFlowCallSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowCallSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::CallSite, graph, inst) {}

  void addCallee(Function *callee) {
    if (!callee)
      return;
    if (std::find(callees_.begin(), callees_.end(), callee) == callees_.end())
      callees_.push_back(callee);
  }
  ArrayRef<Function *> getCallees() const { return callees_; }

  void addCommonInput(GuardedValueFlowNode *node) { common_inputs_.push_back(node); }
  ArrayRef<GuardedValueFlowNode *> getCommonInputs() const {
    return common_inputs_;
  }

  void setCommonOutput(GuardedValueFlowNode *node) { common_output_ = node; }
  GuardedValueFlowNode *getCommonOutput() const { return common_output_; }

  void addPseudoInput(Function *callee, GuardedValueFlowNode *node);
  void addPseudoOutput(Function *callee, GuardedValueFlowNode *node);

  GuardedValueFlowNode *getPseudoInput(Function *callee, unsigned idx) const;
  GuardedValueFlowNode *getPseudoOutput(Function *callee, unsigned idx) const;
  unsigned getNumPseudoInputs(Function *callee) const;
  unsigned getNumPseudoOutputs(Function *callee) const;

private:
  std::vector<Function *> callees_;
  std::vector<GuardedValueFlowNode *> common_inputs_;
  GuardedValueFlowNode *common_output_{nullptr};
  std::map<Function *, std::vector<GuardedValueFlowNode *>> pseudo_inputs_;
  std::map<Function *, std::vector<GuardedValueFlowNode *>> pseudo_outputs_;
};

class GuardedValueFlowDereferenceSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowDereferenceSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::DereferenceSite, graph, inst) {}

  void setPointerOperand(GuardedValueFlowNode *node) { pointer_operand_ = node; }
  void setValueOperand(GuardedValueFlowNode *node) { value_operand_ = node; }
  GuardedValueFlowNode *getPointerOperand() const { return pointer_operand_; }
  GuardedValueFlowNode *getValueOperand() const { return value_operand_; }

private:
  GuardedValueFlowNode *pointer_operand_{nullptr};
  GuardedValueFlowNode *value_operand_{nullptr};
};

} // namespace gvg
} // namespace llvm
