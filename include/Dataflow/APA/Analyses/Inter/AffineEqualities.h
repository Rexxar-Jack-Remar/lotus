#pragma once

#include "Dataflow/APA/Domains/AffineExpression.h"
#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Domains/AffineRelationDomain.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace llvm {
class BasicBlock;
class Function;
class Value;
class Module;
} // namespace llvm

namespace elimination {

struct AffineFunctionKey {
  const llvm::Function *function = nullptr;

  bool operator<(const AffineFunctionKey &other) const {
    return function < other.function;
  }
};

struct AffineBlockKey {
  const llvm::BasicBlock *block = nullptr;

  bool operator<(const AffineBlockKey &other) const {
    return block < other.block;
  }
};

enum class InterAffineVocabularyMode { AllScalars, ObservableSlice };

struct InterAffineEqualitiesOptions {
  explicit InterAffineEqualitiesOptions(
      InterAffineVocabularyMode Vocabulary =
          InterAffineVocabularyMode::AllScalars,
      bool Verbose = false, std::size_t MaxTrackedValues = 0)
      : vocabulary(Vocabulary), verbose(Verbose),
        maxTrackedValues(MaxTrackedValues) {}

  InterAffineVocabularyMode vocabulary;
  bool verbose;
  // Zero means unlimited. Values outside a bounded observable slice are
  // soundly treated as untracked/havoced.
  std::size_t maxTrackedValues;
};

struct InterAffineEqualitiesResult : AffineResultContext {
  SolveStatus status = SolveStatus::Ok;
  std::size_t trackedValues = 0;
  std::map<AffineFunctionKey, AffineRelationDomain::value_type> summaries;
  std::map<AffineBlockKey, AffineRelationDomain::value_type> blockRelations;
};

InterAffineEqualitiesResult runInterElimAffineEqualities(
    llvm::Module &M,
    InterAffineEqualitiesOptions options = InterAffineEqualitiesOptions());

} // namespace elimination
