#include "Analysis/SCCP/SCCP.h"

#include <deque>
#include <set>
#include <utility>

#include <llvm/IR/CFG.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IntrinsicInst.h>

namespace lotus {
namespace analysis {
namespace sccp {

namespace {

using ReadOnlyGlobalMap = llvm::DenseMap<const llvm::GlobalVariable *, SccpValue>;

bool hasSameConstantValue(const llvm::ConstantInt *lhs,
                          const llvm::ConstantInt *rhs) {
  if (lhs == rhs) {
    return true;
  }
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }
  return lhs->getType() == rhs->getType() && lhs->getValue() == rhs->getValue();
}

llvm::Value *toMutableValue(const llvm::Value *value) {
  return const_cast<llvm::Value *>(value);
}

const llvm::Value *stripPointerCasts(const llvm::Value *value) {
  return value == nullptr ? nullptr : value->stripPointerCasts();
}

SccpValue constantToSccpValue(const llvm::Constant *constant) {
  if (constant == nullptr) {
    return SccpValue::getBottom();
  }

  if (llvm::isa<llvm::UndefValue>(constant)) {
    return SccpValue::getBottom();
  }

  if (auto *constant_int = llvm::dyn_cast<llvm::ConstantInt>(constant)) {
    return SccpValue::getConstant(constant_int);
  }

  return SccpValue::getBottom();
}

const llvm::ConstantInt *convertConstantToType(const llvm::ConstantInt *constant,
                                               llvm::Type *type) {
  if (constant == nullptr || type == nullptr) {
    return nullptr;
  }

  auto *integer_type = llvm::dyn_cast<llvm::IntegerType>(type);
  if (integer_type == nullptr) {
    return nullptr;
  }

  if (constant->getType() == integer_type) {
    return constant;
  }

  return llvm::cast<llvm::ConstantInt>(
      llvm::ConstantInt::get(integer_type,
                             constant->getValue().zextOrTrunc(integer_type->getBitWidth())));
}

const llvm::ConstantInt *evaluateBinary(const llvm::BinaryOperator &instruction,
                                        const llvm::ConstantInt *lhs,
                                        const llvm::ConstantInt *rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return nullptr;
  }

  auto *result_type = llvm::dyn_cast<llvm::IntegerType>(instruction.getType());
  if (result_type == nullptr) {
    return nullptr;
  }

  const auto &lhs_value = lhs->getValue();
  const auto &rhs_value = rhs->getValue();
  llvm::APInt result_value(lhs_value);

  switch (instruction.getOpcode()) {
  case llvm::Instruction::Add:
    result_value = lhs_value + rhs_value;
    break;
  case llvm::Instruction::Sub:
    result_value = lhs_value - rhs_value;
    break;
  case llvm::Instruction::Mul:
    result_value = lhs_value * rhs_value;
    break;
  case llvm::Instruction::And:
    result_value = lhs_value & rhs_value;
    break;
  case llvm::Instruction::Or:
    result_value = lhs_value | rhs_value;
    break;
  case llvm::Instruction::Xor:
    result_value = lhs_value ^ rhs_value;
    break;
  case llvm::Instruction::SDiv:
    if (rhs_value.isZero()) {
      return nullptr;
    }
    result_value = lhs_value.sdiv(rhs_value);
    break;
  case llvm::Instruction::SRem:
    if (rhs_value.isZero()) {
      return nullptr;
    }
    result_value = lhs_value.srem(rhs_value);
    break;
  default:
    return nullptr;
  }

  return llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(result_type, result_value));
}

const llvm::ConstantInt *evaluateICmp(const llvm::ICmpInst &instruction,
                                      const llvm::ConstantInt *lhs,
                                      const llvm::ConstantInt *rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return nullptr;
  }

  bool comparison_result = false;
  switch (instruction.getPredicate()) {
  case llvm::CmpInst::ICMP_EQ:
    comparison_result = lhs->getValue() == rhs->getValue();
    break;
  case llvm::CmpInst::ICMP_NE:
    comparison_result = lhs->getValue() != rhs->getValue();
    break;
  case llvm::CmpInst::ICMP_SLT:
    comparison_result = lhs->getValue().slt(rhs->getValue());
    break;
  case llvm::CmpInst::ICMP_SLE:
    comparison_result = lhs->getValue().sle(rhs->getValue());
    break;
  case llvm::CmpInst::ICMP_SGT:
    comparison_result = lhs->getValue().sgt(rhs->getValue());
    break;
  case llvm::CmpInst::ICMP_SGE:
    comparison_result = lhs->getValue().sge(rhs->getValue());
    break;
  default:
    return nullptr;
  }

  return llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(
      llvm::cast<llvm::IntegerType>(instruction.getType()), comparison_result ? 1 : 0));
}

const llvm::ConstantInt *evaluateCast(const llvm::CastInst &instruction,
                                      const llvm::ConstantInt *source) {
  if (source == nullptr) {
    return nullptr;
  }

  auto *target_type = llvm::dyn_cast<llvm::IntegerType>(instruction.getType());
  if (target_type == nullptr) {
    return nullptr;
  }

  switch (instruction.getOpcode()) {
  case llvm::Instruction::ZExt:
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(target_type,
                               source->getValue().zext(target_type->getBitWidth())));
  case llvm::Instruction::SExt:
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(target_type,
                               source->getValue().sext(target_type->getBitWidth())));
  case llvm::Instruction::Trunc:
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(target_type,
                               source->getValue().trunc(target_type->getBitWidth())));
  case llvm::Instruction::BitCast:
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(target_type,
                               source->getValue().zextOrTrunc(target_type->getBitWidth())));
  default:
    return nullptr;
  }
}

ReadOnlyGlobalMap buildReadOnlyGlobals(const llvm::Module &module) {
  llvm::SmallPtrSet<const llvm::GlobalVariable *, 16> stored_globals;

  for (const auto &function : module) {
    if (function.isDeclaration()) {
      continue;
    }

    for (const auto &basic_block : function) {
      for (const auto &instruction : basic_block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        if (store == nullptr) {
          continue;
        }

        auto *global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
            stripPointerCasts(store->getPointerOperand()));
        if (global != nullptr) {
          stored_globals.insert(global);
        }
      }
    }
  }

  ReadOnlyGlobalMap read_only_globals;
  for (const auto &global : module.globals()) {
    if (stored_globals.contains(&global) || !global.hasInitializer()) {
      continue;
    }

    auto value = constantToSccpValue(global.getInitializer());
    if (!value.isBottom()) {
      read_only_globals[&global] = value;
    }
  }

  return read_only_globals;
}

class Solver {
public:
  Solver(const llvm::Function &function, const ReadOnlyGlobalMap &read_only_globals)
      : function_(function), read_only_globals_(read_only_globals) {}

  FunctionResult run(void) {
    if (function_.isDeclaration() || function_.empty()) {
      return {};
    }

    markBlockExecutable(&function_.getEntryBlock());

    while (!cfg_worklist_.empty() || !ssa_worklist_.empty()) {
      while (!cfg_worklist_.empty()) {
        auto *basic_block = cfg_worklist_.front();
        cfg_worklist_.pop_front();
        visitBlock(*basic_block);
      }

      while (!ssa_worklist_.empty()) {
        auto *value = ssa_worklist_.front();
        ssa_worklist_.pop_front();
        visitUsers(*value);
      }
    }

    FunctionResult result;
    for (const auto &entry : value_lattice_) {
      if (entry.second.isConstant()) {
        result.constants[entry.first] = entry.second.getConstant();
      }
    }

    for (const auto &basic_block : function_) {
      if (!executable_blocks_.contains(&basic_block)) {
        result.dead_blocks.insert(&basic_block);
      }
    }

    return result;
  }

private:
  SccpValue getValueState(const llvm::Value &value) const {
    if (llvm::isa<llvm::UndefValue>(&value)) {
      return SccpValue::getBottom();
    }

    if (auto *constant_int = llvm::dyn_cast<llvm::ConstantInt>(&value)) {
      return SccpValue::getConstant(constant_int);
    }

    auto it = value_lattice_.find(toMutableValue(&value));
    if (it != value_lattice_.end()) {
      return it->second;
    }

    return SccpValue::getTop();
  }

  void updateValue(llvm::Value &value, const SccpValue &new_value) {
    auto *mutable_value = &value;
    auto old_it = value_lattice_.find(mutable_value);
    auto old_value = old_it == value_lattice_.end() ? SccpValue::getTop() : old_it->second;
    auto merged_value = old_value.meet(new_value);

    if (merged_value == old_value) {
      return;
    }

    value_lattice_[mutable_value] = merged_value;
    ssa_worklist_.push_back(mutable_value);
  }

  void markBlockExecutable(const llvm::BasicBlock *basic_block) {
    if (basic_block == nullptr) {
      return;
    }

    if (executable_blocks_.insert(basic_block).second) {
      cfg_worklist_.push_back(basic_block);
    }
  }

  void markEdgeExecutable(const llvm::BasicBlock *source,
                          const llvm::BasicBlock *target) {
    if (source == nullptr || target == nullptr) {
      return;
    }

    auto inserted = executable_edges_.insert(std::make_pair(source, target)).second;
    if (!inserted) {
      return;
    }

    executable_blocks_.insert(target);
    cfg_worklist_.push_back(target);
  }

  void visitBlock(const llvm::BasicBlock &basic_block) {
    for (const auto &instruction : basic_block) {
      evaluateInstruction(instruction);
    }
  }

  void visitUsers(llvm::Value &value) {
    for (auto *user : value.users()) {
      auto *instruction = llvm::dyn_cast<llvm::Instruction>(user);
      if (instruction == nullptr) {
        continue;
      }

      if (!executable_blocks_.contains(instruction->getParent())) {
        continue;
      }

      evaluateInstruction(*instruction);
    }
  }

  void evaluateInstruction(const llvm::Instruction &instruction) {
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
      evaluatePhi(*phi);
      return;
    }

    if (auto *icmp = llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
      evaluateICmpInstruction(*icmp);
      return;
    }

    if (auto *binary_operator = llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
      evaluateBinaryInstruction(*binary_operator);
      return;
    }

    if (auto *cast = llvm::dyn_cast<llvm::CastInst>(&instruction)) {
      evaluateCastInstruction(*cast);
      return;
    }

    if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(&instruction)) {
      evaluateBranch(*branch);
      return;
    }

    if (auto *switch_inst = llvm::dyn_cast<llvm::SwitchInst>(&instruction)) {
      evaluateSwitch(*switch_inst);
      return;
    }

    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      evaluateLoad(*load);
      return;
    }

    if (llvm::isa<llvm::AllocaInst>(&instruction)) {
      updateValue(const_cast<llvm::Instruction &>(instruction), SccpValue::getBottom());
      return;
    }

    if (llvm::isa<llvm::GetElementPtrInst>(&instruction)) {
      updateValue(const_cast<llvm::Instruction &>(instruction), SccpValue::getBottom());
      return;
    }

    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
      evaluateCall(*call);
      return;
    }

    if (llvm::isa<llvm::ReturnInst>(&instruction)) {
      return;
    }

    if (!instruction.getType()->isVoidTy()) {
      updateValue(const_cast<llvm::Instruction &>(instruction), SccpValue::getBottom());
    }
  }

  void evaluatePhi(const llvm::PHINode &phi) {
    SccpValue result = SccpValue::getTop();
    for (unsigned index = 0; index < phi.getNumIncomingValues(); ++index) {
      auto *predecessor = phi.getIncomingBlock(index);
      auto edge = std::make_pair(predecessor, phi.getParent());
      if (executable_edges_.find(edge) == executable_edges_.end()) {
        continue;
      }

      result = result.meet(getValueState(*phi.getIncomingValue(index)));
    }

    updateValue(const_cast<llvm::PHINode &>(phi), result);
  }

  void evaluateBinaryInstruction(const llvm::BinaryOperator &instruction) {
    auto lhs = getValueState(*instruction.getOperand(0));
    auto rhs = getValueState(*instruction.getOperand(1));

    if (lhs.isBottom() || rhs.isBottom()) {
      updateValue(const_cast<llvm::BinaryOperator &>(instruction), SccpValue::getBottom());
      return;
    }

    if (!lhs.isConstant() || !rhs.isConstant()) {
      updateValue(const_cast<llvm::BinaryOperator &>(instruction), SccpValue::getTop());
      return;
    }

    auto *folded = evaluateBinary(instruction, lhs.getConstant(), rhs.getConstant());
    if (folded == nullptr) {
      updateValue(const_cast<llvm::BinaryOperator &>(instruction), SccpValue::getBottom());
      return;
    }

    updateValue(const_cast<llvm::BinaryOperator &>(instruction),
                SccpValue::getConstant(folded));
  }

  void evaluateICmpInstruction(const llvm::ICmpInst &instruction) {
    auto lhs = getValueState(*instruction.getOperand(0));
    auto rhs = getValueState(*instruction.getOperand(1));

    if (lhs.isBottom() || rhs.isBottom()) {
      updateValue(const_cast<llvm::ICmpInst &>(instruction), SccpValue::getBottom());
      return;
    }

    if (!lhs.isConstant() || !rhs.isConstant()) {
      updateValue(const_cast<llvm::ICmpInst &>(instruction), SccpValue::getTop());
      return;
    }

    auto *folded = evaluateICmp(instruction, lhs.getConstant(), rhs.getConstant());
    if (folded == nullptr) {
      updateValue(const_cast<llvm::ICmpInst &>(instruction), SccpValue::getBottom());
      return;
    }

    updateValue(const_cast<llvm::ICmpInst &>(instruction), SccpValue::getConstant(folded));
  }

  void evaluateCastInstruction(const llvm::CastInst &instruction) {
    auto source = getValueState(*instruction.getOperand(0));
    if (source.isBottom()) {
      updateValue(const_cast<llvm::CastInst &>(instruction), SccpValue::getBottom());
      return;
    }

    if (!source.isConstant()) {
      updateValue(const_cast<llvm::CastInst &>(instruction), SccpValue::getTop());
      return;
    }

    auto *folded = evaluateCast(instruction, source.getConstant());
    if (folded == nullptr) {
      updateValue(const_cast<llvm::CastInst &>(instruction), SccpValue::getBottom());
      return;
    }

    updateValue(const_cast<llvm::CastInst &>(instruction), SccpValue::getConstant(folded));
  }

  void evaluateBranch(const llvm::BranchInst &instruction) {
    if (!instruction.isConditional()) {
      markEdgeExecutable(instruction.getParent(), instruction.getSuccessor(0));
      return;
    }

    auto condition = getValueState(*instruction.getCondition());
    if (condition.isConstant()) {
      auto *target = condition.getConstant()->isZero() ? instruction.getSuccessor(1)
                                                       : instruction.getSuccessor(0);
      markEdgeExecutable(instruction.getParent(), target);
      return;
    }

    markEdgeExecutable(instruction.getParent(), instruction.getSuccessor(0));
    markEdgeExecutable(instruction.getParent(), instruction.getSuccessor(1));
  }

  void evaluateSwitch(const llvm::SwitchInst &instruction) {
    auto discriminator = getValueState(*instruction.getCondition());
    if (discriminator.isConstant()) {
      auto value = discriminator.getConstant()->getValue();
      for (const auto &case_handle : instruction.cases()) {
        if (case_handle.getCaseValue()->getValue() == value) {
          markEdgeExecutable(instruction.getParent(), case_handle.getCaseSuccessor());
          return;
        }
      }

      markEdgeExecutable(instruction.getParent(), instruction.getDefaultDest());
      return;
    }

    markEdgeExecutable(instruction.getParent(), instruction.getDefaultDest());
    for (const auto &case_handle : instruction.cases()) {
      markEdgeExecutable(instruction.getParent(), case_handle.getCaseSuccessor());
    }
  }

  void evaluateLoad(const llvm::LoadInst &instruction) {
    auto *global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
        stripPointerCasts(instruction.getPointerOperand()));
    if (global == nullptr) {
      updateValue(const_cast<llvm::LoadInst &>(instruction), SccpValue::getBottom());
      return;
    }

    auto it = read_only_globals_.find(global);
    if (it == read_only_globals_.end() || !it->second.isConstant()) {
      updateValue(const_cast<llvm::LoadInst &>(instruction), SccpValue::getBottom());
      return;
    }

    auto *folded = convertConstantToType(it->second.getConstant(), instruction.getType());
    if (folded == nullptr) {
      updateValue(const_cast<llvm::LoadInst &>(instruction), SccpValue::getBottom());
      return;
    }

    updateValue(const_cast<llvm::LoadInst &>(instruction), SccpValue::getConstant(folded));
  }

  void evaluateCall(const llvm::CallBase &instruction) {
    if (instruction.getType()->isVoidTy()) {
      return;
    }

    updateValue(const_cast<llvm::CallBase &>(instruction), SccpValue::getBottom());
  }

  const llvm::Function &function_;
  const ReadOnlyGlobalMap &read_only_globals_;
  llvm::DenseMap<llvm::Value *, SccpValue> value_lattice_;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> executable_blocks_;
  std::set<std::pair<const llvm::BasicBlock *, const llvm::BasicBlock *>> executable_edges_;
  std::deque<const llvm::BasicBlock *> cfg_worklist_;
  std::deque<llvm::Value *> ssa_worklist_;
};

} // namespace

SccpValue::SccpValue(SccpValueKind kind, const llvm::ConstantInt *constant)
    : kind_(kind), constant_(constant) {}

SccpValue SccpValue::getTop(void) { return SccpValue(SccpValueKind::Top, nullptr); }

SccpValue SccpValue::getConstant(const llvm::ConstantInt *constant) {
  return constant == nullptr ? getBottom() : SccpValue(SccpValueKind::Constant, constant);
}

SccpValue SccpValue::getBottom(void) {
  return SccpValue(SccpValueKind::Bottom, nullptr);
}

SccpValueKind SccpValue::getKind(void) const { return kind_; }

bool SccpValue::isTop(void) const { return kind_ == SccpValueKind::Top; }

bool SccpValue::isConstant(void) const { return kind_ == SccpValueKind::Constant; }

bool SccpValue::isBottom(void) const { return kind_ == SccpValueKind::Bottom; }

const llvm::ConstantInt *SccpValue::getConstant(void) const { return constant_; }

SccpValue SccpValue::meet(const SccpValue &other) const {
  if (isTop()) {
    return other;
  }
  if (other.isTop()) {
    return *this;
  }
  if (isBottom() || other.isBottom()) {
    return getBottom();
  }
  if (hasSameConstantValue(constant_, other.constant_)) {
    return *this;
  }
  return getBottom();
}

bool SccpValue::operator==(const SccpValue &other) const {
  return kind_ == other.kind_ && hasSameConstantValue(constant_, other.constant_);
}

bool SccpValue::operator!=(const SccpValue &other) const { return !(*this == other); }

FunctionResult runSCCPOnFunction(const llvm::Function &function) {
  const auto *module = function.getParent();
  ReadOnlyGlobalMap read_only_globals;
  if (module != nullptr) {
    read_only_globals = buildReadOnlyGlobals(*module);
  }
  Solver solver(function, read_only_globals);
  return solver.run();
}

ModuleResult runSCCPOnModule(llvm::Module &module) {
  auto read_only_globals = buildReadOnlyGlobals(module);
  ModuleResult result;

  for (auto &function : module) {
    if (function.isDeclaration()) {
      continue;
    }

    Solver solver(function, read_only_globals);
    auto function_result = solver.run();

    for (const auto &entry : function_result.constants) {
      result.constants[entry.first] = entry.second;
    }
    for (auto *basic_block : function_result.dead_blocks) {
      result.dead_blocks.insert(basic_block);
    }

    result.function_results[&function] = std::move(function_result);
  }

  return result;
}

} // namespace sccp
} // namespace analysis
} // namespace lotus
