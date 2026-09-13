#pragma once

#include "Dataflow/APA/Domains/AffineRelationDomain.h"

namespace elimination {

// Expression and materialized state types for the affine relation domain.
struct AffineExpr {
  bool top = true;
  int64_t constant = 0;
  std::unordered_map<const llvm::Value *, int64_t> terms;
  bool operator==(const AffineExpr &other) const {
    return top == other.top && constant == other.constant &&
           terms == other.terms;
  }
};

struct AffineEquality {
  unsigned bitWidth = 0;
  int64_t constant = 0;
  std::unordered_map<const llvm::Value *, int64_t> terms;
  bool operator==(const AffineEquality &other) const {
    return bitWidth == other.bitWidth && constant == other.constant &&
           terms == other.terms;
  }
};

struct AffineState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, AffineExpr> values;
  std::vector<AffineEquality> equalities;
  bool operator==(const AffineState &other) const {
    return reachable == other.reachable && values == other.values &&
           equalities == other.equalities;
  }
};

AffineState materializeAffineExpressions(const AffineRelation &relation);

} // namespace elimination
