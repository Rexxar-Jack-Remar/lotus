#ifndef ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_FULLCONSTANTPROPAGATION_H_
#define ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_FULLCONSTANTPROPAGATION_H_

#include "Dataflow/Mono/Core/Domain.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace mono {

enum class FullConstantTag {
  Bottom, // unreachable
  Const,
  Top, // unknown
};

struct FullConstantValue {
  FullConstantTag Tag;
  int64_t ConstValue;

  static FullConstantValue bottom() { return {FullConstantTag::Bottom, 0}; }
  static FullConstantValue top() { return {FullConstantTag::Top, 0}; }
  static FullConstantValue constant(int64_t V) { return {FullConstantTag::Const, V}; }

  bool operator==(const FullConstantValue &Other) const {
    return Tag == Other.Tag && ConstValue == Other.ConstValue;
  }
};

struct FullConstantPropagationState {
  bool Unreachable;
  std::unordered_map<const llvm::Value *, FullConstantValue> Values;

  FullConstantPropagationState() : Unreachable(true), Values() {}

  bool empty() const { return Unreachable; }
};

struct IntraMonoFullConstantPropagationDomain
    : LLVMMonoAnalysisDomain<FullConstantPropagationState> {};

std::unordered_map<llvm::Instruction *, FullConstantPropagationState>
runIntraMonoFullConstantPropagation(llvm::Function *F);

} // namespace mono

#endif // ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_FULLCONSTANTPROPAGATION_H_
