#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/APInt.h>

namespace llvm {
class Value;
class raw_ostream;
} // namespace llvm

namespace elimination {

using AffineRow = std::vector<llvm::APInt>;
using AffineMatrix = std::vector<AffineRow>;
using MOSTransformerSet = std::vector<AffineMatrix>;

enum class AffineStateSide { Pre, Post };

struct AffineQueryTerm {
  const llvm::Value *value;
  llvm::APInt coefficient;
  AffineStateSide side = AffineStateSide::Post;
};

struct AffineRelationVocabulary {
  std::vector<const llvm::Value *> values;
  std::unordered_map<const llvm::Value *, unsigned> indices;
  std::unordered_map<const llvm::Value *, unsigned> actualBitWidths;
  std::vector<const llvm::Value *> localValues;
};

struct AffineRelationComponent {
  unsigned bitWidth = 0;
  AffineMatrix constraints;

  bool operator==(const AffineRelationComponent &other) const;
};

struct AffineRelation {
  bool bottom = false;
  bool identity = false;
  std::map<unsigned, AffineRelationComponent> components;

  bool operator==(const AffineRelation &other) const;
};

/// A symbolic value of an output expression on the relation's feasible inputs.
/// Exact means constant + sum(terms) equals the requested expression for every
/// transition. condition contains only pre-state constraints. Unknown includes
/// non-functional expressions and modular equations with non-invertible pivots.
struct AffineExpressionPrecondition {
  enum class Status { Exact, Unknown, Unreachable };
  Status status = Status::Unknown;
  llvm::APInt constant{1, 0};
  std::vector<std::pair<const llvm::Value *, llvm::APInt>> terms;
  AffineRelation condition;
};

struct AffineCacheStatistics {
  std::size_t normalizations = 0;
  std::size_t normalizationHits = 0;
  std::size_t compositions = 0;
  std::size_t compositionHits = 0;
};

struct AffineGeneratorRelation {
  AffineRelation relation;
  std::map<unsigned, AffineMatrix> generators;
  bool bottom = false;
  bool exact = true;

  bool operator==(const AffineGeneratorRelation &other) const;
};

struct AffineDiagonalDecomposition {
  unsigned bitWidth = 0;
  AffineMatrix left;
  AffineMatrix leftInverse;
  AffineMatrix diagonal;
  AffineMatrix right;
  AffineMatrix rightInverse;
  AffineMatrix dual;
  bool exact = true;

  bool operator==(const AffineDiagonalDecomposition &other) const;
};

struct MOSRelation {
  enum class ConversionKind { Direct, HavocPreStateGuards, MakeExplicit };

  AffineRelation relation;
  std::map<unsigned, MOSTransformerSet> transformers;
  ConversionKind kind = ConversionKind::Direct;
  bool exact = true;

  bool operator==(const MOSRelation &other) const;
};

class AffineRelationDomain {
public:
  using value_type = AffineRelation;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool project_newton_safe = true;

  static void configure(const AffineRelationVocabulary *vocabulary);
  static const AffineRelationVocabulary *getVocabulary();
  /// Per-thread bounded caches; zero disables caching. Reconfiguration clears
  /// entries so matrices are never reused across different vocabularies.
  static void setCacheCapacity(std::size_t entries);
  static AffineCacheStatistics cacheStatistics();

  static bool isTrackedValue(const llvm::Value *value);
  static unsigned bitWidthOf(const llvm::Value *value);
  static unsigned componentBitWidth();
  static unsigned indexOf(const llvm::Value *value);

  static value_type zero();
  static value_type bottom() { return zero(); }
  static value_type top();
  static value_type one();
  static bool equal(const value_type &lhs, const value_type &rhs);
  static bool isBottom(const value_type &relation);
  static bool contains(const value_type &lhs, const value_type &rhs);
  /// Does sum(terms) == constant hold modulo 2^componentBitWidth()?
  /// Coefficients and constant must have that width; values must be tracked.
  /// Invalid queries throw std::invalid_argument, including on bottom.
  /// Bottom entails every valid equation. Pre/post terms may be mixed.
  static bool entails(const value_type &relation, const llvm::APInt &constant,
                      const std::vector<AffineQueryTerm> &terms);
  /// Return a uniquely determined value at its LLVM integer width, or nullopt
  /// for an untracked value, bottom, or a nonconstant value. Queries use the
  /// currently configured vocabulary, as do the other domain operations.
  static std::optional<llvm::APInt>
  getConstant(const value_type &relation, const llvm::Value *value,
              AffineStateSide side = AffineStateSide::Post);
  /// Print modular equations with explicit pre/post variable names.
  static void print(const value_type &relation, llvm::raw_ostream &out);
  /// Pull back constant + sum(terms); all terms use componentBitWidth().
  static AffineExpressionPrecondition
  expressionPrecondition(const value_type &relation,
                         const llvm::APInt &constant,
                         const std::vector<AffineQueryTerm> &terms);
  /// Combine single-destination assignment relations as simultaneous writes.
  /// Every RHS reads the same pre-state. Other post-state columns in each
  /// assignment are existentially removed. Duplicate destinations are invalid.
  static value_type
  parallelAssign(const std::vector<std::pair<const llvm::Value *, value_type>>
                     &assignments);
  static value_type meet(const value_type &lhs, const value_type &rhs);
  static value_type combine(const value_type &lhs, const value_type &rhs);
  static value_type join(const value_type &lhs, const value_type &rhs) {
    return combine(lhs, rhs);
  }
  static value_type ndetCombine(const value_type &lhs, const value_type &rhs);
  static value_type condCombine(bool /*phi*/, const value_type &t,
                                const value_type &e);
  static value_type extend(const value_type &outer, const value_type &inner);
  static value_type extend_lin(const value_type &outer,
                               const value_type &inner);
  static value_type subtract(const value_type &lhs, const value_type &rhs);
  static value_type project(const value_type &relation);

  static value_type identity();
  static value_type addStateConstraint(
      const value_type &relation, int64_t constant,
      const std::vector<std::pair<const llvm::Value *, int64_t>> &terms);
  static value_type addPrecondition(const value_type &relation,
                                    const llvm::Value *value, int64_t constant);
  static value_type makeForget(const llvm::Value *dest);
  static value_type havoc(const value_type &relation, const llvm::Value *value);
  static value_type havoc(const value_type &relation,
                          const std::vector<const llvm::Value *> &values);
  static value_type
  projectOnto(const value_type &relation,
              const std::vector<const llvm::Value *> &keepValues);
  static value_type
  mergePreservingLocals(const value_type &callSite,
                        const value_type &calleeExit,
                        const std::vector<const llvm::Value *> &locals);
  static llvm::APInt size(const value_type &relation);
  static value_type makeAffineAssignment(
      const llvm::Value *dest, int64_t constant,
      const std::vector<std::pair<const llvm::Value *, int64_t>> &terms);
  static value_type makeAffineCongruenceAssignment(
      const llvm::Value *dest, unsigned modulusBits, int64_t constant,
      const std::vector<std::pair<const llvm::Value *, int64_t>> &terms);

  static AffineGeneratorRelation toAffineGenerator(const value_type &relation);
  static value_type
  fromAffineGenerator(const AffineGeneratorRelation &relation);
  static AffineGeneratorRelation
  joinAffineGenerators(const AffineGeneratorRelation &lhs,
                       const AffineGeneratorRelation &rhs);
  static AffineDiagonalDecomposition
  diagonalDecompose(const AffineMatrix &matrix, unsigned bitWidth);
  static AffineMatrix dualizePerp(const AffineMatrix &matrix, unsigned bitWidth,
                                  unsigned columns);

  static MOSRelation toMOS(const value_type &relation);
  static MOSRelation toMOSWithHavocedPreStateGuards(const value_type &relation);
  static MOSRelation toMOSWithMakeExplicit(const value_type &relation);
  static value_type fromMOS(const MOSRelation &relation);
  static MOSRelation joinMOS(const MOSRelation &lhs, const MOSRelation &rhs);

private:
  static thread_local AffineRelationVocabulary Vocabulary;
  static thread_local bool HasVocabulary;
  static thread_local unsigned ConfiguredBitWidth;
};

/// Restore the calling thread's domain configuration on scope exit.
class ScopedAffineVocabulary {
public:
  explicit ScopedAffineVocabulary(const AffineRelationVocabulary &vocabulary);
  ~ScopedAffineVocabulary();
  ScopedAffineVocabulary(const ScopedAffineVocabulary &) = delete;
  ScopedAffineVocabulary &operator=(const ScopedAffineVocabulary &) = delete;

private:
  std::optional<AffineRelationVocabulary> previous;
};

/// Analysis results retain column meanings. LLVM values themselves remain
/// owned by the caller's module, which must outlive queries on the result.
struct AffineResultContext {
  AffineRelationVocabulary vocabulary;
  ScopedAffineVocabulary scopedVocabulary() const {
    return ScopedAffineVocabulary(vocabulary);
  }
  std::optional<llvm::APInt>
  getConstant(const AffineRelation &relation, const llvm::Value *value,
              AffineStateSide side = AffineStateSide::Post) const {
    auto scope = scopedVocabulary();
    return AffineRelationDomain::getConstant(relation, value, side);
  }
  bool entails(const AffineRelation &relation, const llvm::APInt &constant,
               const std::vector<AffineQueryTerm> &terms) const {
    auto scope = scopedVocabulary();
    return AffineRelationDomain::entails(relation, constant, terms);
  }
  void print(const AffineRelation &relation, llvm::raw_ostream &out) const {
    auto scope = scopedVocabulary();
    AffineRelationDomain::print(relation, out);
  }
};

} // namespace elimination

