/** @file PropertyValue.h @brief Abstract property value representation for symbolic analysis. */
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

/// Base class for symbolic properties attached to program values and memory.
///
/// A `PropertyValue` is the arithmetic layer of the symbolic execution engine.
/// Analysis state uses these objects for offsets, sizes, scalar facts, and
/// path-sensitive comparisons. Concrete integers and affine symbolic
/// expressions share this interface so transfer functions can propagate values
/// without committing to one representation too early.
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

  /// Path-sensitive comparison used when a client wants a three-way answer.
  /// Returns 0 for false, 1 for true, and 2 when the current representation is
  /// not precise enough to decide.
  unsigned cmp(const PropertyValue *Rhs, unsigned Pred) const;

  // handle the NumRes = 2 case
  // std::vector<std::tuple<const PropertyValue *, const PropertyValue *, const
  // PropertyValue *>> underApproxOp(const PropertyValue *Rhs, BinOp Op) const;

  PropertyValueKind getKind() const { return ValKind; }

  /// Renames symbolic variables according to `M`.
  ///
  /// This is the main hook used by call/return summary application and by state
  /// rewrites that move facts from one symbolic context to another.
  virtual PropertyValuePtr map(const std::unordered_map<Var, Var> &M,
                               bool Total = true) const;
  virtual PropertyValuePtr mapWithDefault(const std::unordered_map<Var, Var> &M,
                                          Var DefaultV) const;

  /// Substitutes symbolic variables with already computed property values.
  /// Clients use this when simplifying state fragments or instantiating summary
  /// facts with concrete caller-side expressions.
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

/// Shared wrapper used throughout the engine to pass property values by value.
///
/// `PropertyValuePtr` gives transfer functions lightweight algebraic syntax and
/// structural equality while keeping the underlying objects immutable to most
/// clients. This is the value type that appears in access paths, memory object
/// sizes, constraint construction, and summary materialization.
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
