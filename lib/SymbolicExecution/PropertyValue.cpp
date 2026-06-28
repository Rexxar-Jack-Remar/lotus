
#include "SymbolicExecution/PropertyValue.h"

#include "SymbolicExecution/ProgramVar.h"
#include "SymbolicExecution/PropertyAllocator.h"
#include "SymbolicExecution/PropertyInteger.h"
#include "SymbolicExecution/PropertySym.h"

#include <numeric>

using namespace SymbolicExecution;

PropertyValue::PropertyValue(PropertyValueKind valK) : ValKind(valK) {}

PropertyValue::BinOp PropertyValue::fromLLVMOp(unsigned OpCode) {
  if (OpCode == Instruction::Add) {
    return Add;
  } else if (OpCode == Instruction::Sub) {
    return Sub;
  } else if (OpCode == Instruction::Mul) {
    return Mul;
  } else if (OpCode == Instruction::SDiv) {
    return SDiv;
  } else if (OpCode == Instruction::UDiv) {
    return UDiv;
  } else {
    assert(false);
  }
}

unsigned PropertyValue::cmp(const PropertyValue *Rhs, unsigned Pred) const {
  bool isConstant = false;
  APInt LeftVal, RightVal;
  if (isa<PropertyInteger>(this) && isa<PropertyInteger>(Rhs)) {
    LeftVal = cast<PropertyInteger>(this)->getVal().getVal();
    RightVal = cast<PropertyInteger>(Rhs)->getVal().getVal();
    isConstant = true;
  } else {
    PropertyValuePtr ResVal(this->binOp(Rhs, PropertyValue::Sub));
    if (ResVal && IsaProperty<PropertyInteger>(ResVal)) {
      LeftVal = CastProperty<PropertyInteger>(ResVal)->getVal().getVal();
      RightVal = (LeftVal.getBitWidth(), 0);
      isConstant = true;
    }
  }

  int CmpRes = -1;
  if (isConstant) {
    if (LeftVal.getBitWidth() > RightVal.getBitWidth()) {
      RightVal = RightVal.sext(LeftVal.getBitWidth());
    } else if (LeftVal.getBitWidth() < RightVal.getBitWidth()) {
      LeftVal = LeftVal.sext(RightVal.getBitWidth());
    }

    switch (Pred) {
    case CmpInst::ICMP_EQ:
      CmpRes = (LeftVal == RightVal);
      break;
    case CmpInst::ICMP_NE:
      CmpRes = (LeftVal != RightVal);
      break;
    // has to use signed comparison because LeftVal may be negative.
    case CmpInst::ICMP_SGE:
    case CmpInst::ICMP_UGE:
      CmpRes = (LeftVal.sge(RightVal));
      break;
    case CmpInst::ICMP_SGT:
    case CmpInst::ICMP_UGT:
      CmpRes = (LeftVal.sgt(RightVal));
      break;
    case CmpInst::ICMP_SLE:
    case CmpInst::ICMP_ULE:
      CmpRes = (LeftVal.sle(RightVal));
      break;
    case CmpInst::ICMP_SLT:
    case CmpInst::ICMP_ULT:
      CmpRes = (LeftVal.slt(RightVal));
      break;
    default:
      assert(false);
    }

  } else {
    CmpRes = 2;
  }

  assert(CmpRes == 0 || CmpRes == 1 || CmpRes == 2);

  return (unsigned)CmpRes;
}

PropertyValuePtr PropertyValue::binOp(const PropertyValue &Rhs,
                                      BinOp Op) const {
  return binOp(&Rhs, Op);
}

PropertyValuePtr PropertyValue::binOp(const PropertyValuePtr &Rhs,
                                      BinOp Op) const {
  return binOp(ToRaw(Rhs), Op);
}

PropertyValuePtr PropertyValue::binOp(const PropertyValue *Rhs,
                                      BinOp Op) const {
  auto LValKind = getKind();
  auto RValKind = Rhs->getKind();

  if (LValKind == VK_Integer && RValKind == VK_Integer) {
    const PropertyInteger *Left = cast<PropertyInteger>(this);
    const PropertyInteger *Right = cast<PropertyInteger>(Rhs);

    if (Right->getVal() == 0) {
      if (Op == UDiv || Op == SDiv) {
        return PropertyValuePtr();
      }
    }

    return GetProperty<PropertyInteger>(
        Left->getVal().doBinOp(Right->getVal(), Op));
  } else if (LValKind == VK_SymExpr && RValKind == VK_SymExpr) {
    const PropertySymExpr *Left = cast<PropertySymExpr>(this);
    const PropertySymExpr *Right = cast<PropertySymExpr>(Rhs);
    assert(!Left->isConstant());
    assert(!Right->isConstant());

    PropertySymExpr Res;
    if (Op == PropertyValue::Add) {
      Res = *Left + *Right;
    } else if (Op == PropertyValue::Sub) {
      Res = *Left - *Right;
    } else {
      return PropertyValuePtr(); // zero value
    }

    if (Res.isConstant()) {
      return GetProperty<PropertyInteger>(Res.getAsConstant());
    } else {
      return GetProperty<PropertySymExpr>(std::move(Res));
    }
  } else if (LValKind == VK_Integer && RValKind == VK_SymExpr) {
    const PropertyInteger *Left = cast<PropertyInteger>(this);
    const PropertySymExpr *Right = cast<PropertySymExpr>(Rhs);
    assert(!Right->isConstant());

    PropertySymExpr Res;
    if (Op == PropertyValue::Add) {
      Res = *Right + Left->getVal();
    } else if (Op == PropertyValue::Sub) {
      Res = *Right * (-1) + Left->getVal();
    } else if (Op == PropertyValue::Mul) {
      Res = *Right * Left->getVal();
    } else {
      return PropertyValuePtr(); // zero value
    }

    if (Res.isConstant()) { // can happen if times zero
      return GetProperty<PropertyInteger>(Res.getAsConstant());
    } else {
      return GetProperty<PropertySymExpr>(std::move(Res));
    }
  } else if (LValKind == VK_SymExpr && RValKind == VK_Integer) {
    const PropertySymExpr *Left = cast<PropertySymExpr>(this);
    const PropertyInteger *Right = cast<PropertyInteger>(Rhs);
    assert(!Left->isConstant());

    PropertySymExpr Res;
    if (Op == PropertyValue::Add) {
      Res = *Left + Right->getVal();
    } else if (Op == PropertyValue::Sub) {
      Res = *Left - Right->getVal();
    } else if (Op == PropertyValue::Mul) {
      Res = *Left * Right->getVal();
    } else {
      return PropertyValuePtr(); // zero value
    }

    if (Res.isConstant()) { // can happen if times zero
      return GetProperty<PropertyInteger>(Res.getAsConstant());
    } else {
      return GetProperty<PropertySymExpr>(std::move(Res));
    }
  } else {
    assert(false);
  }

  return PropertyValuePtr();
}

PropertyValuePtr PropertyValue::map(const std::unordered_map<Var, Var> &M,
                                    bool) const {
  return PropertyValuePtr(std::shared_ptr<PropertyValue>(clone()));
}

PropertyValuePtr
PropertyValue::mapWithDefault(const std::unordered_map<Var, Var> &M,
                              Var DefaultV) const {
  return map(M);
}

PropertyValuePtr
PropertyValue::map(const std::unordered_map<Var, PropertyValuePtr> &M) const {
  return PropertyValuePtr(std::shared_ptr<PropertyValue>(clone()));
}

void PropertyValue::dump() const { dumpDbgString(llvm::errs()); }

std::string PropertyValue::getName() const {
  if (ValKind == VK_Integer) {
    auto Val = cast<PropertyInteger>(this)->getAsBoundInt();
    return std::to_string(Val);
  } else {
    const auto *Val = cast<PropertySymExpr>(this);
    std::string Name = "Sym";
    if (Val->isVar()) {
      ProgramValuePtr V = Val->getAsVar().getValue();
      if (V.isa<GuardedValueFlowNodeValue>()) {
        Name = V.getLLVMVal()->getName();
      } else {
        Name = cast<AuxValue>(V.getData().get())->getName();
      }
    }
    return Name;
  }
}

PropertyValuePtr::PropertyValuePtr(const std::shared_ptr<PropertyInteger> &V)
    : Data(V) {}

PropertyValuePtr::PropertyValuePtr(const std::shared_ptr<PropertySymExpr> &V)
    : Data(V) {}

PropertyValuePtr::PropertyValuePtr(const ProgramValuePtr &PV) {
  if (PV.isVacuous()) {
    // Handle vacuous (null) values
    Data = nullptr;
    return;
  }

  try {
    // Create a copy to avoid potential issues
    ProgramValuePtr PVCopy = PV;

    // Double-check after copy
    if (PVCopy.isVacuous()) {
      Data = nullptr;
      return;
    }

    Var V(std::move(PVCopy));

    if (V.getValue().isVacuous()) {
      // Var ended up vacuous somehow
      Data = nullptr;
      return;
    }

    if (V.isConstant()) {
      try {
        Data = GetProperty<PropertyInteger>(V.getAsConstant());
      } catch (...) {
        llvm::errs() << "Error: PropertyInteger creation failed\n";
        Data = nullptr;
      }
    } else {
      try {
        Data = GetProperty<PropertySymExpr>(V);
      } catch (...) {
        llvm::errs() << "Error: PropertySymExpr creation failed\n";
        Data = nullptr;
      }
    }
  } catch (...) {
    llvm::errs() << "Error: PropertyValuePtr construction failed\n";
    Data = nullptr;
  }
}

bool PropertyValuePtr::operator==(const PropertyValuePtr &R) const {
  if (Data == R.Data) {
    return true;
  }

  if (!Data || !R.Data) {
    return false;
  }

  return Data->isEquivalent(*R);
}

size_t PropertyValuePtr::hash() const {
  if (!Data) {
    return std::hash<void *>()(nullptr);
  } else {
    return Data->hash();
  }
}

PropertyValuePtr PropertyValuePtr::operator-() const {
  return PropertyInteger(0).binOp(get(), PropertyValue::Sub);
}

bool PropertyValuePtr::operator<(int64_t V) const {
  if (IsaProperty<PropertyInteger>(*this) &&
      CastProperty<PropertyInteger>(*this)->getVal() < V) {
    return true;
  }
  return false;
}

bool PropertyValuePtr::operator<=(int64_t V) const {
  if (IsaProperty<PropertyInteger>(*this) &&
      CastProperty<PropertyInteger>(*this)->getVal() <= V) {
    return true;
  }
  return false;
}

bool PropertyValuePtr::operator==(int64_t V) const {
  if (IsaProperty<PropertyInteger>(*this) &&
      CastProperty<PropertyInteger>(*this)->getVal() == V) {
    return true;
  }
  return false;
}

bool PropertyValuePtr::operator>(int64_t V) const {
  if (IsaProperty<PropertyInteger>(*this) &&
      CastProperty<PropertyInteger>(*this)->getVal() > V) {
    return true;
  }
  return false;
}

bool PropertyValue::isEquivalent(const PropertyValue &Rhs) const {
  auto LK = getKind(), RK = Rhs.getKind();
  if (LK != RK) {
    return false;
  }

  if (LK == VK_Integer) {
    return *cast<PropertyInteger>(this) == *cast<PropertyInteger>(&Rhs);
  } else {
    assert(LK == VK_SymExpr);
    return *cast<PropertySymExpr>(this) == *cast<PropertySymExpr>(&Rhs);
  }
}
