

#include "Analysis/SymbolicExecution/ProgramVar.h"

#include "llvm/IR/Constants.h"

#include "Analysis/SymbolicExecution/SegUtility.h"

#include <sstream>

using namespace SymbolicExecution;

std::string GuardedValueFlowNodeValue::getID() const {
  return seg_utility::ptrToString(N);
}

size_t GuardedValueFlowNodeValue::hash() const {
  return std::hash<GuardedValueFlowNode *>()(N);
}

Value *GuardedValueFlowNodeValue::getValue() const {
  if (!N) {
    return nullptr;
  }
  return N->getLLVMValue();
}

AuxValue::AuxValue(Type *Ty, std::string N)
    : ProgramValue(VK_AUX), Ty(Ty), Name(std::move(N)) {}

size_t AuxValue::hash() const {
  return seg_utility::hashHelper(
      {std::hash<Type *>()(Ty), std::hash<std::string>()(Name)});
}

ProgramValuePtr::ProgramValuePtr(const GuardedValueFlowNode *V) {
  if (!V) {
    Data = nullptr;
    return;
  }
  Data = std::unique_ptr<GuardedValueFlowNodeValue>(
      new GuardedValueFlowNodeValue((GuardedValueFlowNode *)V));
}

ProgramValuePtr::ProgramValuePtr(Type *Ty, std::string Name) {
  Data = std::unique_ptr<AuxValue>(new AuxValue(Ty, std::move(Name)));
}

ProgramValuePtr::ProgramValuePtr(const ProgramValuePtr &R) {
  if (R.isVacuous()) {
    Data = nullptr;
    return;
  }

  const ProgramValue *Ptr = R.Data.get();
  if (!Ptr) {
    // Safety check: Data pointer is null even though isVacuous() returned false
    Data = nullptr;
    return;
  }

  if (R.isa<GuardedValueFlowNodeValue>()) {
    Data = std::unique_ptr<GuardedValueFlowNodeValue>(
        new GuardedValueFlowNodeValue(*cast<GuardedValueFlowNodeValue>(Ptr)));
  } else {
    Data = std::unique_ptr<AuxValue>(new AuxValue(*cast<AuxValue>(Ptr)));
  }
}

bool ProgramValuePtr::isConstant() const {
  if (!isa<GuardedValueFlowNodeValue>()) {
    return false;
  }

  Value *Val = getLLVMVal();

  if (Val) {
    if (llvm::isa<ConstantInt>(Val) || llvm::isa<ConstantPointerNull>(Val)) {
      return true;
    }
  }

  return false;
}

BigInteger ProgramValuePtr::getAsConstant() const {
  if (isVacuous() || !isa<GuardedValueFlowNodeValue>()) {
    // Return a default value for non-constant cases
    return BigInteger(APInt(64, 0, true));
  }

  Value *Val = getLLVMVal();
  if (!Val) {
    // Return a default value for null LLVM values
    return BigInteger(APInt(64, 0, true));
  }

  if (llvm::isa<ConstantInt>(Val)) {
    return cast<ConstantInt>(Val)->getValue();
  } else if (llvm::isa<ConstantPointerNull>(Val)) {
    // a NULL LLVM value takes 64 bits.
    return BigInteger(APInt(64, 0, true));
  } else {
    // Unexpected value type - return default
    llvm::errs() << "Warning: getAsConstant() called on non-constant value\n";
    return BigInteger(APInt(64, 0, true));
  }
}

void ProgramValuePtr::dump() const {
  if (isVacuous()) {
    llvm::errs() << "(vacuous)";
    return;
  }
  const ProgramValue *Val = Data.get();
  if (Val->getValKind() == ProgramValue::VK_GVFG) {
    auto *Node = cast<GuardedValueFlowNodeValue>(Val)->getNode();
    if (Value *llvm_value = Node ? Node->getLLVMValue() : nullptr)
      llvm::errs() << *llvm_value << "\n";
    else if (Node)
      llvm::errs() << Node->getDescription() << "\n";
  } else {
    llvm::errs() << cast<AuxValue>(Val)->getName() << "\n";
  }
}

ProgramValuePtr ProgramValuePtr::operator=(ProgramValuePtr R) {
  std::swap(Data, R.Data);
  return *this;
}

Value *ProgramValuePtr::getLLVMVal() const {
  if (!isa<GuardedValueFlowNodeValue>()) {
    return nullptr;
  }
  return cast<GuardedValueFlowNodeValue>(Data.get())->getValue();
}

Type *ProgramValuePtr::getType() const {
  if (isVacuous()) {
    return nullptr;
  }

  if (isa<GuardedValueFlowNodeValue>()) {
    return cast<GuardedValueFlowNodeValue>(Data.get())->getNode()->getType();
  } else {
    return cast<AuxValue>(Data.get())->getType();
  }
}
