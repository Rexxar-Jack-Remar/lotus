

#include "Analysis/SymbolicExecution/PropertySym.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringExtras.h"

#include "Analysis/SymbolicExecution/PropertyAllocator.h"
#include "Analysis/SymbolicExecution/PropertyInteger.h"
#include "Analysis/SymbolicExecution/PropertyValue.h"
#include "Analysis/SymbolicExecution/GVFGUtility.h"

using namespace SymbolicExecution;

PropertySymExpr::PropertySymExpr(Var V) : PropertyValue(VK_SymExpr) {
  addTerm(V, 1);
}

PropertySymExpr::PropertySymExpr(Var V, BigInteger C)
    : PropertyValue(VK_SymExpr) {
  addTerm(V, C);
}

PropertySymExpr::PropertySymExpr() : PropertyValue(VK_SymExpr) {}

void PropertySymExpr::addTerm(Var V, BigInteger C) {
  if (V.getValue().isVacuous()) {
    llvm::errs() << "Warning: addTerm called with vacuous Var, skipping\n";
    return;
  }

  if (V.isConstant()) {
    // If the value is constant, add it to the constant offset instead
    offsets += C * V.getAsConstant();
    return;
  }

  coeffs[V] += C;

  if (coeffs[V] == 0) {
    coeffs.erase(V);
  }
}

void PropertySymExpr::setConstTerm(const BigInteger &C) { offsets = C; }

void PropertySymExpr::addConstTerm(const BigInteger &C) { offsets += C; }

void PropertySymExpr::dumpDbgString(raw_ostream &O) const {
  if (isConstant()) {
    APInt Offset = offsets.getVal();
    O << "sym:" << llvm::toString(Offset, 10, true) << "\n";
    return;
  }

  std::string VPrefix = "X";
  unsigned Idx = 0;
  std::string Rep;
  std::map<unsigned, ProgramValuePtr> IdxToValue;

  for (const auto &P : coeffs) {
    std::string CoeffStr;
    if (P.second != 1) {
      CoeffStr = llvm::toString(P.second.getVal(), 10, true);
    }

    Rep += CoeffStr + VPrefix + std::to_string(Idx);

    if (Idx != coeffs.size() - 1) {
      Rep += "+";
    }

    IdxToValue.insert(std::make_pair(Idx, P.first.getValue()));
    ++Idx;
  }

  if (offsets != 0) {
    Rep += "+";
    Rep += llvm::toString(offsets.getVal(), 10, true);
  }

  O << "sym:" + Rep << "\n";
  for (const auto &P : IdxToValue) {
    std::string ValS = VPrefix + std::to_string(P.first) + "=";
    O << ValS;
    O << P.second.getID();
    O << "\n";
  }
}

Function *PropertySymExpr::getEnclosingFunc() const {
  if (isConstant()) {
    return nullptr;
  }

  Function *Func = nullptr;
  for (auto Iter = coeffs.begin(), EIter = coeffs.end(); Iter != EIter;
       ++Iter) {
    Function *CurFunc = gvfg_utility::getEnclosingFunc(Iter->first);
    if (CurFunc) {
      if (!Func) {
        Func = CurFunc;
      } else {
        if (Func == CurFunc) {
          continue;
        } else {
          llvm::errs() << "[Bug] symexpr with vars from different functions.\n";
          return nullptr;
        }
      }
    }
  }

  return Func;
}

PropertySymExpr PropertySymExpr::operator+(const PropertySymExpr &R) const {
  PropertySymExpr ResExpr(*this);
  for (auto Iter = R.coeffs.begin(), EIter = R.coeffs.end(); Iter != EIter;
       ++Iter) {
    ResExpr.addTerm(Iter->first, Iter->second);
  }

  ResExpr.offsets += R.offsets;
  return ResExpr;
}

PropertySymExpr PropertySymExpr::operator-(const PropertySymExpr &R) const {
  PropertySymExpr ResExpr(*this);
  for (auto Iter = R.coeffs.begin(), EIter = R.coeffs.end(); Iter != EIter;
       ++Iter) {
    ResExpr.addTerm(Iter->first, -Iter->second);
  }

  ResExpr.offsets -= R.offsets;
  return ResExpr;
}

PropertySymExpr PropertySymExpr::operator*(const BigInteger &R) const {
  if (R == 0) {
    return PropertySymExpr();
  }

  if (R == 1) {
    return *this;
  }

  PropertySymExpr ResExpr;
  for (auto Iter = begin(), EIter = end(); Iter != EIter; ++Iter) {
    auto X = Iter->first;
    auto C = Iter->second * R; // overflow may happen
    ResExpr.addTerm(X, C);
  }

  ResExpr.addConstTerm(this->offsets * R);

  return ResExpr;
}

PropertySymExpr PropertySymExpr::operator+(const BigInteger &R) const {
  PropertySymExpr ResExpr(*this);
  ResExpr.offsets += R;
  return ResExpr;
}

PropertySymExpr PropertySymExpr::operator-(const BigInteger &R) const {
  PropertySymExpr ResExpr(*this);
  ResExpr.offsets -= R;
  return ResExpr;
}

PropertySymExpr PropertySymExpr::doBinOp(const BigInteger &R, BinOp Op) const {
  if (Op == Add) {
    return this->operator+(R);
  } else if (Op == Sub) {
    return this->operator-(R);
  } else {
    assert(Op == Mul);
    return this->operator*(R);
  }
}

std::unordered_set<Var> PropertySymExpr::getUsedVars() const {
  std::unordered_set<Var> UsedVars;
  for (const auto &P : coeffs) {
    UsedVars.insert(P.first);
  }
  return UsedVars;
}

bool PropertySymExpr::isVar() const {
  if (offsets == 0) {
    if (coeffs.size() == 1) {
      if (coeffs.begin()->second == 1) {
        return true;
      }
    }
  }
  return false;
}

Var PropertySymExpr::getAsVar() const {
  assert(isVar());
  return coeffs.begin()->first;
}

PropertyValuePtr
PropertySymExpr::map(const std::unordered_map<Var, PropertyValuePtr> &M) const {
  PropertySymExpr MappedExpr;
  BigInteger ExtraOffsets = 0;

  MappedExpr.offsets = offsets;
  for (auto Iter = coeffs.begin(), EIter = coeffs.end(); Iter != EIter;
       ++Iter) {
    Var FromV(Iter->first);
    if (M.count(FromV)) {
      auto TermVal =
          M.at(FromV)->binOp(PropertyInteger(Iter->second), PropertyValue::Mul);
      assert(TermVal);
      if (IsaProperty<PropertyInteger>(TermVal)) {
        ExtraOffsets += CastProperty<PropertyInteger>(TermVal)->getVal();
      } else {
        MappedExpr = MappedExpr + *CastProperty<PropertySymExpr>(TermVal);
      }
    } else {
      MappedExpr.addTerm(Iter->first, Iter->second);
    }
  }

  MappedExpr.offsets += ExtraOffsets;
  if (MappedExpr.isConstant()) {
    return GetProperty<PropertyInteger>(MappedExpr.getAsConstant());
  } else {
    return GetProperty<PropertySymExpr>(std::move(MappedExpr));
  }
}

PropertyValuePtr PropertySymExpr::map(const std::unordered_map<Var, Var> &M,
                                      bool Total) const {
  PropertySymExpr MappedExpr;
  BigInteger ExtraOffsets = 0;

  MappedExpr.offsets = offsets;
  for (auto Iter = coeffs.begin(), EIter = coeffs.end(); Iter != EIter;
       ++Iter) {
    Var FromV(Iter->first);
    if (M.count(FromV)) {
      Var MapedV = M.at(FromV);

      if (MapedV.isConstant()) {
        ExtraOffsets += MapedV.getAsConstant() * Iter->second;
      } else {
        MappedExpr.addTerm(MapedV, Iter->second);
      }
    } else {
      if (Total) {
        return PropertyValuePtr();
      } else {
        MappedExpr.addTerm(Iter->first, Iter->second);
      }
    }
  }

  MappedExpr.offsets += ExtraOffsets;
  if (MappedExpr.isConstant()) {
    return GetProperty<PropertyInteger>(MappedExpr.getAsConstant());
  } else {
    return GetProperty<PropertySymExpr>(std::move(MappedExpr));
  }
}

PropertyValuePtr
PropertySymExpr::mapWithDefault(const std::unordered_map<Var, Var> &M,
                                Var DefaultV) const {
  auto Res = map(M);
  if (Res) {
    return Res;
  } else {
    if (DefaultV.isConstant()) {
      return GetProperty<PropertyInteger>(DefaultV.getAsConstant());
    } else {
      return GetProperty<PropertySymExpr>(DefaultV);
    }
  }
}

size_t PropertySymExpr::hash() const {
  std::vector<size_t> Hashes;
  for (auto Iter = begin(), EIter = end(); Iter != EIter; ++Iter) {
    Hashes.push_back(std::hash<Var>()(Iter->first));
    Hashes.push_back(llvm::hash_value(Iter->second.getVal()));
  }
  Hashes.push_back(llvm::hash_value(offsets.getVal()));

  return gvfg_utility::hashHelper(Hashes);
}
