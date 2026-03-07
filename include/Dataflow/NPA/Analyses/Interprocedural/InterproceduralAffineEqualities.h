#ifndef NPA_INTERPROC_AFFINE_EQUALITIES_H
#define NPA_INTERPROC_AFFINE_EQUALITIES_H

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"
#include "Dataflow/NPA/Domains/ProgramTransferDomain.h"

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

struct AffineState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, AffineExpr> values;

  bool operator==(const AffineState &other) const {
    return reachable == other.reachable && values == other.values;
  }
};

struct AffineOp {
  enum class Kind {
    AssignConst,
    Copy,
    Add,
    Sub,
    Scale,
    Select,
    Phi,
    Forget,
  };

  Kind kind = Kind::Forget;
  const llvm::Value *dest = nullptr;
  const llvm::Value *lhs = nullptr;
  const llvm::Value *rhs = nullptr;
  const llvm::Value *cond = nullptr;
  int64_t constant = 0;
  std::vector<const llvm::Value *> inputs;

  bool operator<(const AffineOp &other) const;
  bool operator==(const AffineOp &other) const;
};

using AffineDomain = ProgramTransferDomain<AffineOp>;

class InterproceduralAffineEqualities {
public:
  struct Result {
    std::map<FunctionKey, AffineDomain::value_type> summaries;
    std::map<BlockKey, AffineState> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false);
};

} // namespace npa

#endif // NPA_INTERPROC_AFFINE_EQUALITIES_H
