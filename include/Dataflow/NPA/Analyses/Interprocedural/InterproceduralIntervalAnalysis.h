#ifndef NPA_INTERPROC_INTERVAL_ANALYSIS_H
#define NPA_INTERPROC_INTERVAL_ANALYSIS_H

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

struct Interval {
  bool bottom = false;
  bool hasLower = false;
  bool hasUpper = false;
  int64_t lower = 0;
  int64_t upper = 0;

  static Interval top() { return {}; }
  static Interval point(int64_t value) {
    Interval out;
    out.hasLower = true;
    out.hasUpper = true;
    out.lower = value;
    out.upper = value;
    return out;
  }

  bool operator==(const Interval &other) const {
    return bottom == other.bottom && hasLower == other.hasLower &&
           hasUpper == other.hasUpper && lower == other.lower &&
           upper == other.upper;
  }
};

struct IntervalState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, Interval> values;

  bool operator==(const IntervalState &other) const {
    return reachable == other.reachable && values == other.values;
  }
};

struct IntervalOp {
  enum class Kind {
    AssignConst,
    Copy,
    Cast,
    Binary,
    Compare,
    Select,
    Phi,
    Forget,
  };

  Kind kind = Kind::Forget;
  const llvm::Value *dest = nullptr;
  const llvm::Value *lhs = nullptr;
  const llvm::Value *rhs = nullptr;
  const llvm::Value *cond = nullptr;
  unsigned opcode = 0;
  unsigned bitWidth = 0;
  unsigned sourceBitWidth = 0;
  int64_t constant = 0;
  std::vector<const llvm::Value *> inputs;

  bool operator<(const IntervalOp &other) const;
  bool operator==(const IntervalOp &other) const;
};

using IntervalDomain = ProgramTransferDomain<IntervalOp>;

class InterproceduralIntervalAnalysis {
public:
  struct Result {
    std::map<FunctionKey, IntervalDomain::value_type> summaries;
    std::map<BlockKey, IntervalState> blockFacts;
  };

  static Result run(llvm::Module &M, bool verbose = false);
};

} // namespace npa

#endif // NPA_INTERPROC_INTERVAL_ANALYSIS_H
