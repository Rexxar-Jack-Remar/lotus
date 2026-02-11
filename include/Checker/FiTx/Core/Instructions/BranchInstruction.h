#pragma once
#include <climits>
#include <map>
#include <vector>

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Instructions/CallInstruction.h"
#include "Checker/FiTx/Core/Value.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"

namespace framework {

class CompareInst : public Instruction {
 public:
  static std::shared_ptr<CompareInst> Create(
      llvm::ICmpInst* load_inst,
      std::vector<Fields> fields = std::vector<Fields>(),
      long array_element_num = Value::kNonArrayElement);

  CompareInst(llvm::ICmpInst* instruction, std::vector<Fields> fields,
              long array_element_num);

  const std::vector<std::shared_ptr<framework::Value>>& Operands() const {
    return operands_;
  }

  void setOperands(std::vector<std::shared_ptr<framework::Value>> operand);
  void replaceOperand(const std::shared_ptr<framework::Value>& operand,
                      const std::shared_ptr<framework::Value>& new_operand);
  bool operandExists(const std::shared_ptr<framework::Value>& value);
  bool operandExists(const std::shared_ptr<framework::CallInst>& call_inst);
  bool returnValueOperandExists();

  std::shared_ptr<framework::Value> find_if(
      std::function<bool(std::shared_ptr<framework::Value>)> lambda);

  const llvm::CmpInst::Predicate GetPredicate() const { return predicate_; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static bool classof(const framework::Instruction* I) {
    return I->Opcode() == llvm::Instruction::ICmp;
  }

  static bool classof(const framework::Value* V) {
    return llvm::isa<framework::Instruction>(V) &&
           classof(llvm::cast<framework::Instruction>(V));
  }

 private:
  llvm::CmpInst::Predicate predicate_;
  std::vector<std::shared_ptr<framework::Value>> operands_;
  std::vector<std::shared_ptr<framework::Value>> replaced_;
};

class BranchInst : public Instruction {
 public:
  using TransitionNodes = std::vector<std::weak_ptr<framework::BasicBlock>>;
  constexpr static int64_t kTrueTransition = 0;
  constexpr static int64_t kFalseTransition = INT64_MIN;

  static std::shared_ptr<BranchInst> Create(
      llvm::SwitchInst* load_inst,
      std::vector<Fields> field = std::vector<Fields>(),
      long array_element_num = Value::kNonArrayElement);

  static std::shared_ptr<BranchInst> Create(
      llvm::BranchInst* load_inst,
      std::vector<Fields> field = std::vector<Fields>(),
      long array_element_num = Value::kNonArrayElement);

  BranchInst(llvm::BranchInst* instruction, std::vector<Fields> fields,
             long array_element_num);
  BranchInst(llvm::SwitchInst* instruction, std::vector<Fields> fields,
             long array_element_num);

  std::shared_ptr<framework::Instruction> Condition() {
    return condition_instruction_;
  }

  void setCondition(std::shared_ptr<framework::Instruction> inst) {
    condition_instruction_ = inst;
  }

  void setTransitionNode(int64_t code,
                         const std::shared_ptr<framework::BasicBlock>& block);

  bool isInOperand(const std::shared_ptr<framework::Value>& value);
  bool returnValueOperandExists();
  TransitionNodes TruePathNodes() { return nodes_[kTrueTransition]; };
  TransitionNodes FalsePathNodes() { return nodes_[kFalseTransition]; };

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static bool classof(const framework::Instruction* I) {
    return I->Opcode() == llvm::Instruction::Br ||
           I->Opcode() == llvm::Instruction::Switch;
  }

  static bool classof(const framework::Value* V) {
    return llvm::isa<framework::Instruction>(V) &&
           classof(llvm::cast<framework::Instruction>(V));
  }

 private:
  std::shared_ptr<framework::Instruction> condition_instruction_;
  std::map<int64_t, TransitionNodes> nodes_;
};
}  // namespace framework
