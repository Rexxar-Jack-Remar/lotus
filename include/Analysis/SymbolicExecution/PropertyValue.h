#ifndef ANALYSIS_SYMBOLICEXECUTION_PROPERTYVALUE_H
#define ANALYSIS_SYMBOLICEXECUTION_PROPERTYVALUE_H
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_os_ostream.h"

#include <list>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

namespace SymbolicExecution {

class PropertyValue;
class PropertyInteger;
class PropertySymExpr;
class Var;

class PropertyValuePtr;
class PropertyValue {
public:
  enum BinOp { Add, Sub, Mul, UDiv, SDiv };

  static BinOp fromLLVMOp(unsigned OpCode);

public:
  enum PropertyValueKind {
    VK_Integer, // c
    VK_SymExpr  // a1x1+...+anxn+c,  all ai != 0 && all xi not constant
  };

  PropertyValue(PropertyValueKind valK);
  virtual ~PropertyValue() = default;

  // NumRes = 1: the operation is exact. Return a newly allocated value for the
  // result. NumRes = 0: no possible value can be derived for the result of the
  // computation. Return nullptr. NumRes = 2: there exists > 1 results. Return
  // nullptr because in this case the computation is not exact. XXDBG const
  // PropertyValue *binOp(const PropertyValue *Rhs, BinOp Op, unsigned &NumRes)
  // const;

  PropertyValuePtr binOp(const PropertyValue *Rhs, BinOp Op) const;
  PropertyValuePtr binOp(const PropertyValuePtr &Rhs, BinOp Op) const;
  PropertyValuePtr binOp(const PropertyValue &Rhs, BinOp Op) const;

  // 0 - false; 1 - true; 2 -unknown
  unsigned cmp(const PropertyValue *Rhs, unsigned Pred) const;

  // handle the NumRes = 2 case
  // std::vector<std::tuple<const PropertyValue *, const PropertyValue *, const
  // PropertyValue *>> underApproxOp(const PropertyValue *Rhs, BinOp Op) const;

  PropertyValueKind getKind() const { return ValKind; }

  virtual PropertyValuePtr map(const std::unordered_map<Var, Var> &M,
                               bool Total = true) const;
  virtual PropertyValuePtr mapWithDefault(const std::unordered_map<Var, Var> &M,
                                          Var DefaultV) const;
  virtual PropertyValuePtr
  map(const std::unordered_map<Var, PropertyValuePtr> &M) const;

  virtual size_t hash() const = 0;
  virtual PropertyValue *clone() const = 0;

  bool isEquivalent(const PropertyValue &Rhs) const;

  virtual void dumpDbgString(raw_ostream &) const { return; }

  void dump() const;

  std::string getName() const;

  virtual Function *getEnclosingFunc() const { return nullptr; }

private:
  PropertyValueKind ValKind;
};

class ProgramValuePtr;
class PropertyValuePtr {
public:
  PropertyValuePtr() {}
  PropertyValuePtr(const std::shared_ptr<PropertyValue> &V) : Data(V) {}
  PropertyValuePtr(const std::shared_ptr<PropertyInteger> &V);
  PropertyValuePtr(const std::shared_ptr<PropertySymExpr> &V);
  PropertyValuePtr(const ProgramValuePtr &V);
  PropertyValuePtr(const PropertyValuePtr &) = default;
  PropertyValuePtr &operator=(const PropertyValuePtr &) = default;

  bool operator==(const PropertyValuePtr &R) const;

  size_t hash() const;

  const PropertyValue *operator->() const { return get(); }

  const PropertyValue &operator*() const { return *get(); }

  operator bool() const { return Data != nullptr; }

  const PropertyValue *get() const { return Data.get(); }

  PropertyValuePtr operator+(const PropertyValuePtr &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Add);
  }

  PropertyValuePtr operator+(const PropertyValue &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Add);
  }

  PropertyValuePtr operator-(const PropertyValuePtr &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Sub);
  }

  PropertyValuePtr operator-(const PropertyValue &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Sub);
  }

  PropertyValuePtr operator-() const;

  PropertyValuePtr operator*(const PropertyValuePtr &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Mul);
  }

  PropertyValuePtr operator*(const PropertyValue &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Mul);
  }

  bool operator<(int64_t V) const;
  bool operator<=(int64_t V) const;
  bool operator==(int64_t V) const;
  bool operator>(int64_t V) const;

private:
  std::shared_ptr<const PropertyValue> Data = nullptr;
};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::PropertyValuePtr> {
  size_t operator()(const SymbolicExecution::PropertyValuePtr &V) const {
    return V.hash();
  }
};
} // namespace std

#endif
