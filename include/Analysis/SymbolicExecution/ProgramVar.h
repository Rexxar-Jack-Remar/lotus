#ifndef ANALYSIS_SYMBOLICEXECUTION_PROGRAMVAR_H
#define ANALYSIS_SYMBOLICEXECUTION_PROGRAMVAR_H

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_os_ostream.h"

#include "Analysis/SymbolicExecution/BigInteger.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"

#include <list>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

namespace SymbolicExecution {
using lotus::gvfg::GuardedValueFlowNode;

/// Base class for symbolic execution values that can appear as variable roots.
///
/// The engine reasons about program state through a small set of stable value
/// identifiers instead of raw LLVM SSA nodes alone. A ProgramValue either wraps
/// a GVFG node, which ties the symbolic state to the guarded value-flow graph,
/// or represents an auxiliary placeholder introduced by the analysis for
/// synthetic memory objects, summaries, or temporary symbolic names.
class ProgramValue {
public:
  enum ValKind { VK_GVFG, VK_AUX };
  ProgramValue(ValKind VK) : VK(VK) {}

  virtual ~ProgramValue() {}

  ValKind getValKind() const { return VK; }

  virtual size_t hash() const { return std::hash<unsigned>()(VK); }

  virtual std::string getID() const = 0;

protected:
  ValKind VK;
};

// class LLVMVal : public ProgramValue {
// public:
//   LLVMVal(Value *V) : ProgramValue(VK_LLVM), V(V) {}
//
//   operator Value *() const { return V; }
//
//   static bool classof(const ProgramValue *O) {
//     return O->getValKind() == VK_LLVM;
//   }
//
//   Value *getValue() const { return V; }
//
//   bool operator==(const LLVMVal &R) const { return V == R.V; }
//
//   bool operator<(const LLVMVal &R) const { return V < R.V; }
//
//   virtual size_t hash() const override { return std::hash<Value *>()(V); }
//
//   std::string getID() const override;
//
// private:
//   Value *V;
// };

class GuardedValueFlowNodeValue : public ProgramValue {
public:
  GuardedValueFlowNodeValue(GuardedValueFlowNode *N)
      : ProgramValue(VK_GVFG), N(N) {}

  static bool classof(const ProgramValue *O) {
    return O->getValKind() == VK_GVFG;
  }

  Value *getValue() const;

  GuardedValueFlowNode *getNode() const { return N; }

  bool operator==(const GuardedValueFlowNodeValue &R) const { return N == R.N; }

  bool operator<(const GuardedValueFlowNodeValue &R) const { return N < R.N; }

  size_t hash() const override;

  std::string getID() const override;

private:
  GuardedValueFlowNode *N;
};

class AuxValue : public ProgramValue {
public:
  AuxValue(Type *Ty, std::string Name);

  static bool classof(const ProgramValue *O) {
    return O->getValKind() == VK_AUX;
  }

  Type *getType() const { return Ty; }

  std::string getName() const { return Name; }

  std::string getID() const override { return Name; }

  bool operator==(const AuxValue &R) const {
    return Ty == R.Ty && Name == R.Name;
  }

  bool operator<(const AuxValue &R) const {
    if (Ty == R.Ty) {
      return Name < R.Name;
    }
    return Ty < R.Ty;
  }

  size_t hash() const override;

private:
  Type *Ty;
  std::string Name;
};

class ProgramValuePtr {
public:
  ProgramValuePtr() : Data(nullptr) {}
  ProgramValuePtr(const GuardedValueFlowNode *V);
  ProgramValuePtr(Type *Ty, std::string Name);
  ProgramValuePtr(const ProgramValuePtr &R);
  ProgramValuePtr(ProgramValuePtr &&R) : Data(std::move(R.Data)) {}
  ProgramValuePtr operator=(ProgramValuePtr R);

  bool isNull() const { return Data == nullptr; }

  /// Returns the underlying LLVM value when this wrapper names a GVFG-backed
  /// program value. Auxiliary values intentionally have no direct LLVM peer.
  Value *getLLVMVal() const;

  /// Returns the LLVM type carried by the wrapped program value.
  ///
  /// For GVFG-backed values this comes from the underlying IR value. For
  /// auxiliary values it comes from the synthetic type supplied at creation.
  Type *getType() const;

  bool isVacuous() const { return Data == nullptr; }

  template <typename Ty> const Ty *getAs() const {
    return cast<Ty>(Data.get());
  }

  template <typename Ty> bool isa() const {
    return Data != nullptr && llvm::isa<Ty>(Data.get());
  }

  // template <typename Ty> bool isaLLVM() const {
  //   return isLLVMVal() && isa<Ty>(getLLVMVal());
  // }

  bool operator==(const ProgramValuePtr &R) const {
    if (isVacuous() || R.isVacuous()) {
      return Data == R.Data;
    } else if (isa<GuardedValueFlowNodeValue>() &&
               R.isa<GuardedValueFlowNodeValue>()) {
      return *getAs<GuardedValueFlowNodeValue>() ==
             *R.getAs<GuardedValueFlowNodeValue>();
    } else if (!isa<GuardedValueFlowNodeValue>() &&
               !R.isa<GuardedValueFlowNodeValue>()) {
      return *getAs<AuxValue>() == *R.getAs<AuxValue>();
    } else {
      return false;
    }
  }

  bool operator<(const ProgramValuePtr &R) const {
    if (isVacuous() || R.isVacuous()) {
      return Data < R.Data;
    } else if (isa<GuardedValueFlowNodeValue>() &&
               R.isa<GuardedValueFlowNodeValue>()) {
      return *getAs<GuardedValueFlowNodeValue>() <
             *R.getAs<GuardedValueFlowNodeValue>();
    } else if (!isa<GuardedValueFlowNodeValue>() &&
               !R.isa<GuardedValueFlowNodeValue>()) {
      return *getAs<AuxValue>() < *R.getAs<AuxValue>();
    } else {
      return (isa<GuardedValueFlowNodeValue>() &&
              !R.isa<GuardedValueFlowNodeValue>());
    }
  }

  const std::unique_ptr<ProgramValue> &getData() const { return Data; }

  std::string getID() const { return Data->getID(); }

  size_t hash() const { return isVacuous() ? 0 : Data->hash(); }

  bool isConstant() const;

  /// Extracts a concrete integer when the wrapped value is recognized as a
  /// constant leaf in the symbolic state.
  BigInteger getAsConstant() const;

  void dump() const;

private:
  std::unique_ptr<ProgramValue> Data;
};

class Var {
public:
  explicit Var(GuardedValueFlowNode *V) : Data(V) {}
  explicit Var(ProgramValuePtr V) : Data(std::move(V)) {}
  Var(const Var &) = default;
  Var(Var &&) = default;

  Var &operator=(Var R) {
    Data = std::move(R.Data);
    return *this;
  }

  bool operator==(const Var &R) const { return Data == R.Data; }

  bool operator<(const Var &R) const { return Data < R.Data; }

  /// Returns the IR value that seeded this symbolic variable, if any.
  ///
  /// Many higher-level components, including `PropertySymExpr`, path-condition
  /// construction, and bug reporting, use `Var` as the canonical handle for
  /// symbolic variables while still being able to recover the originating LLVM
  /// instruction or argument when one exists.
  inline Value *getLLVMValue() const { return Data.getLLVMVal(); }

  /// Returns the underlying program-level identity used by the symbolic state.
  ProgramValuePtr getValue() const { return Data; }

  bool isConstant() const { return Data.isConstant(); }

  BigInteger getAsConstant() const { return Data.getAsConstant(); }

private:
  ProgramValuePtr Data;
};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::Var> {
  size_t operator()(const SymbolicExecution::Var &V) const {
    return V.getValue().getData()->hash();
  }
};
template <> struct hash<SymbolicExecution::ProgramValuePtr> {
  size_t operator()(const SymbolicExecution::ProgramValuePtr &V) const {
    return V.hash();
  }
};

} // namespace std

#endif
