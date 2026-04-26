#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_SIGNANALYSIS_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_SIGNANALYSIS_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Adapters/LLVM/ForwardProblem.h"

#include <cstdint>
#include <unordered_map>

namespace elimination {

class SignValue final {
public:
  enum Bits : std::uint8_t { None = 0, Negative = 1, Zero = 2, Positive = 4 };

  SignValue() = default;
  explicit SignValue(std::uint8_t Mask) : Mask(Mask) {}

  static SignValue bottom() { return SignValue(None); }
  static SignValue negative() { return SignValue(Negative); }
  static SignValue zero() { return SignValue(Zero); }
  static SignValue positive() { return SignValue(Positive); }
  static SignValue nonNegative() { return SignValue(Zero | Positive); }
  static SignValue nonZero() { return SignValue(Negative | Positive); }
  static SignValue top() { return SignValue(Negative | Zero | Positive); }

  bool isBottom() const { return Mask == None; }
  bool mayBeNegative() const { return (Mask & Negative) != 0; }
  bool mayBeZero() const { return (Mask & Zero) != 0; }
  bool mayBePositive() const { return (Mask & Positive) != 0; }
  std::uint8_t bits() const { return Mask; }

  void mergeIn(SignValue Other) { Mask |= Other.Mask; }

  friend bool operator==(SignValue Lhs, SignValue Rhs) {
    return Lhs.Mask == Rhs.Mask;
  }
  friend bool operator!=(SignValue Lhs, SignValue Rhs) { return !(Lhs == Rhs); }

private:
  std::uint8_t Mask = None;
};

using SignMap = std::unordered_map<const llvm::Value *, SignValue>;

using SignAnalysisResult =
    DataFlowResultT<llvm::Instruction *, SignMap, llvm::Instruction *>;

SignAnalysisResult runIntraElimSignAnalysis(llvm::Function *F,
                                            EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_SIGNANALYSIS_H_
