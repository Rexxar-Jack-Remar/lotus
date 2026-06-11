/** @file PropertySym.h @brief Symbolic abstract property value for symbolic execution. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_PROPERTYSYM_H
#define ANALYSIS_SYMBOLICEXECUTION_PROPERTYSYM_H

#include "Analysis/SymbolicExecution/BigInteger.h"
#include "Analysis/SymbolicExecution/ProgramVar.h"
#include "Analysis/SymbolicExecution/PropertyValue.h"

#include <map>

namespace SymbolicExecution {

/// Affine symbolic expression used by the symbolic executor.
///
/// The current property domain models values as `a1*x1 + ... + an*xn + c`,
/// where each `xi` is a `Var` and `c` is a `BigInteger` constant. This keeps
/// offsets, sizes, and scalar equalities cheap to rewrite and compare, while
/// still preserving enough structure for path-condition generation and summary
/// application.
class PropertySymExpr : public PropertyValue {
public:
  PropertySymExpr(Var V);
  PropertySymExpr(Var V, BigInteger C);
  PropertySymExpr();

  static bool classof(const PropertyValue *V) {
    return V->getKind() == VK_SymExpr;
  }

  PropertySymExpr operator+(const PropertySymExpr &R) const;
  PropertySymExpr operator-(const PropertySymExpr &R) const;
  PropertySymExpr operator*(const BigInteger &R) const;
  PropertySymExpr operator+(const BigInteger &R) const;
  PropertySymExpr operator-(const BigInteger &R) const;
  PropertySymExpr doBinOp(const BigInteger &R, BinOp Op) const;

  /// True when the affine form has no symbolic variables and therefore reduces
  /// to a concrete integer constant.
  bool isConstant() const { return coeffs.empty(); }

  BigInteger getAsConstant() const {
    assert(isConstant());
    return offsets;
  }

  /// True when the expression is exactly one symbolic variable plus zero.
  bool isVar() const;

  /// Returns that symbolic variable in the degenerate single-variable case.
  Var getAsVar() const;

  PropertyValuePtr map(const std::unordered_map<Var, Var> &M,
                       bool Total = true) const override;
  PropertyValuePtr
  map(const std::unordered_map<Var, PropertyValuePtr> &M) const override;
  PropertyValuePtr mapWithDefault(const std::unordered_map<Var, Var> &M,
                                  Var DefaultV) const override;

  std::map<Var, BigInteger>::iterator begin() { return coeffs.begin(); }

  std::map<Var, BigInteger>::iterator end() { return coeffs.end(); }

  std::map<Var, BigInteger>::const_iterator begin() const {
    return coeffs.begin();
  }

  std::map<Var, BigInteger>::const_iterator end() const { return coeffs.end(); }

  BigInteger getOffsets() const { return offsets; }

  size_t hash() const override;

  /// Enumerates the symbolic variables referenced by this expression.
  /// This is used by callers that need to recover dependencies before solving,
  /// summary export, or taint-to-value correlation.
  std::unordered_set<Var> getUsedVars() const;

  bool operator==(const PropertySymExpr &R) const {
    return coeffs == R.coeffs && offsets == R.offsets;
  }

  /// Adds or updates one affine term. Zero coefficients are normalized away by
  /// the implementation so downstream clients see a canonical form.
  void addTerm(Var var, BigInteger C);
  void setConstTerm(const BigInteger &C);
  void addConstTerm(const BigInteger &C);
  void dumpDbgString(raw_ostream &O) const override;

  const std::map<Var, BigInteger> &getCoeffs() const { return coeffs; }

  Function *getEnclosingFunc() const override;

  PropertyValue *clone() const override { return new PropertySymExpr(*this); }

private:
  // Ordered by the underlying symbolic variable identity so hashing, equality,
  // and debug rendering remain stable across users of the expression.
  std::map<Var, BigInteger> coeffs;
  BigInteger offsets = 0;
};
} // namespace SymbolicExecution
#endif
