#ifndef NPA_INTERPROC_AFFINE_EQUALITIES_H
#define NPA_INTERPROC_AFFINE_EQUALITIES_H

#include "Dataflow/NPA/Analyses/InterEngine.h"
#include "Dataflow/NPA/Domains/AffineRelationDomain.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace llvm {
class Value;
class Module;
} // namespace llvm

namespace npa {

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

class InterAffineEqualities {
public:
  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, AffineRelationDomain::value_type> summaries;
    std::map<BlockKey, AffineRelationDomain::value_type> blockRelations;
  };

  static Result run(llvm::Module &M, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible);
};

AffineState
materializeAffineExpressions(const AffineRelationDomain::value_type &relation);

} // namespace npa

#endif // NPA_INTERPROC_AFFINE_EQUALITIES_H
