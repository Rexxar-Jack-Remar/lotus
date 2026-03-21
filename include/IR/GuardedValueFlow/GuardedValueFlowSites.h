#pragma once

#include "IR/GuardedValueFlow/ConditionRef.h"

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
    Div,
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

class GuardedValueFlowAllocSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowAllocSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Alloc, graph, inst) {}
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
  void setInputSummaryNode(unsigned summary_index, GuardedValueFlowNode *node) {
    input_summary_nodes_[summary_index] = node;
  }
  GuardedValueFlowNode *getInputSummaryNode(unsigned summary_index) const {
    auto it = input_summary_nodes_.find(summary_index);
    return it == input_summary_nodes_.end() ? nullptr : it->second;
  }
  void setOutputSummaryNode(unsigned summary_index, GuardedValueFlowNode *node) {
    output_summary_nodes_[summary_index] = node;
  }
  GuardedValueFlowNode *getOutputSummaryNode(unsigned summary_index) const {
    auto it = output_summary_nodes_.find(summary_index);
    return it == output_summary_nodes_.end() ? nullptr : it->second;
  }
  void setCalleeCondition(Function *callee, ConditionRef condition) {
    if (callee)
      callee_conditions_[callee] = condition;
  }
  bool hasCalleeCondition(Function *callee) const {
    return callee && callee_conditions_.find(callee) != callee_conditions_.end();
  }
  ConditionRef getCalleeCondition(Function *callee) const {
    auto it = callee_conditions_.find(callee);
    return it == callee_conditions_.end() ? ConditionRef::none() : it->second;
  }

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
  std::map<unsigned, GuardedValueFlowNode *> input_summary_nodes_;
  std::map<unsigned, GuardedValueFlowNode *> output_summary_nodes_;
  std::map<Function *, ConditionRef> callee_conditions_;
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

class GuardedValueFlowReturnSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowReturnSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::ReturnSite, graph, inst) {}
};

class GuardedValueFlowGEPReferenceSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowGEPReferenceSite(GuardedValueFlowGraph *graph,
                                   Instruction *inst)
      : GuardedValueFlowSite(Kind::GEP, graph, inst) {}

  void setPointerOperand(GuardedValueFlowNode *node) { pointer_operand_ = node; }
  GuardedValueFlowNode *getPointerOperand() const { return pointer_operand_; }
  void addOffsetOperand(GuardedValueFlowNode *node) { offset_operands_.push_back(node); }
  ArrayRef<GuardedValueFlowNode *> getOffsetOperands() const {
    return offset_operands_;
  }
  void setResultNode(GuardedValueFlowNode *node) { result_node_ = node; }
  GuardedValueFlowNode *getResultNode() const { return result_node_; }

private:
  GuardedValueFlowNode *pointer_operand_{nullptr};
  GuardedValueFlowNode *result_node_{nullptr};
  std::vector<GuardedValueFlowNode *> offset_operands_;
};

class GuardedValueFlowCompareSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowCompareSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Compare, graph, inst) {}

  void setLhsOperand(GuardedValueFlowNode *node) { lhs_operand_ = node; }
  void setRhsOperand(GuardedValueFlowNode *node) { rhs_operand_ = node; }
  GuardedValueFlowNode *getLhsOperand() const { return lhs_operand_; }
  GuardedValueFlowNode *getRhsOperand() const { return rhs_operand_; }

private:
  GuardedValueFlowNode *lhs_operand_{nullptr};
  GuardedValueFlowNode *rhs_operand_{nullptr};
};

class GuardedValueFlowDivSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowDivSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Div, graph, inst) {}

  void setLhsOperand(GuardedValueFlowNode *node) { lhs_operand_ = node; }
  void setRhsOperand(GuardedValueFlowNode *node) { rhs_operand_ = node; }
  GuardedValueFlowNode *getLhsOperand() const { return lhs_operand_; }
  GuardedValueFlowNode *getRhsOperand() const { return rhs_operand_; }

private:
  GuardedValueFlowNode *lhs_operand_{nullptr};
  GuardedValueFlowNode *rhs_operand_{nullptr};
};

} // namespace gvg
} // namespace llvm
