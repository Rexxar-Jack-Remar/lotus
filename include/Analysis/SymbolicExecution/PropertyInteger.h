#ifndef ANALYSIS_SYMBOLICEXECUTION_PROPERTYINTEGER_H
#define ANALYSIS_SYMBOLICEXECUTION_PROPERTYINTEGER_H

#include "Analysis/SymbolicExecution/BigInteger.h"
#include "Analysis/SymbolicExecution/PropertyValue.h"

namespace SymbolicExecution {
class PropertyInteger : public PropertyValue {
public:
  PropertyInteger(BigInteger V);

  static bool classof(const PropertyValue *V) {
    return V->getKind() == VK_Integer;
  }

  BigInteger getVal() const { return Val; }

  size_t hash() const override;

  bool operator==(const PropertyInteger &R) const { return Val == R.Val; }

  int64_t getAsBoundInt() const { return Val.getAsBoundInt(); }

  void dumpDbgString(raw_ostream &O) const override;

  PropertyValue *clone() const override { return new PropertyInteger(*this); }

private:
  BigInteger Val;
};
} // namespace SymbolicExecution
#endif
