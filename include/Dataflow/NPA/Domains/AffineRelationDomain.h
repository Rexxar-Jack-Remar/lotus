#ifndef NPA_AFFINE_RELATION_DOMAIN_H
#define NPA_AFFINE_RELATION_DOMAIN_H

#include "Dataflow/NPA/Core/Base/Runtime.h"

#include <map>
#include <vector>

#include <llvm/ADT/APInt.h>

namespace llvm {
class Value;
} // namespace llvm

namespace npa {

struct AffineRelationVocabulary {
  std::vector<const llvm::Value *> values;
  std::unordered_map<const llvm::Value *, unsigned> indices;
  std::unordered_map<const llvm::Value *, unsigned> actualBitWidths;
  std::vector<const llvm::Value *> localValues;
};

struct AffineRelationComponent {
  unsigned bitWidth = 0;
  std::vector<std::vector<llvm::APInt>> constraints;

  bool operator==(const AffineRelationComponent &other) const;
};

struct AffineRelation {
  bool bottom = false;
  std::map<unsigned, AffineRelationComponent> components;

  bool operator==(const AffineRelation &other) const;
};

class AffineRelationDomain {
public:
  using value_type = AffineRelation;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool project_newton_safe = true;

  static void configure(const AffineRelationVocabulary *vocabulary);
  static const AffineRelationVocabulary *getVocabulary();

  static bool isTrackedValue(const llvm::Value *value);
  static unsigned bitWidthOf(const llvm::Value *value);
  static unsigned componentBitWidth();
  static unsigned indexOf(const llvm::Value *value);

  static value_type zero();
  static value_type one();
  static bool equal(const value_type &lhs, const value_type &rhs);
  static value_type meet(const value_type &lhs, const value_type &rhs);
  static value_type combine(const value_type &lhs, const value_type &rhs);
  static value_type ndetCombine(const value_type &lhs, const value_type &rhs);
  static value_type condCombine(bool /*phi*/, const value_type &t,
                                const value_type &e);
  static value_type extend(const value_type &outer, const value_type &inner);
  static value_type extend_lin(const value_type &outer, const value_type &inner);
  static value_type subtract(const value_type &lhs, const value_type &rhs);
  static value_type project(const value_type &relation);

  static value_type identity();
  static value_type
  addStateConstraint(const value_type &relation, int64_t constant,
                     const std::vector<std::pair<const llvm::Value *, int64_t>>
                         &terms);
  static value_type addPrecondition(const value_type &relation,
                                    const llvm::Value *value, int64_t constant);
  static value_type makeForget(const llvm::Value *dest);
  static value_type havoc(const value_type &relation, const llvm::Value *value);
  static value_type
  havoc(const value_type &relation,
        const std::vector<const llvm::Value *> &values);
  static value_type
  projectOnto(const value_type &relation,
              const std::vector<const llvm::Value *> &keepValues);
  static value_type
  mergePreservingLocals(const value_type &callSite,
                        const value_type &calleeExit,
                        const std::vector<const llvm::Value *> &locals);
  static llvm::APInt size(const value_type &relation);
  static value_type
  makeAffineAssignment(const llvm::Value *dest, int64_t constant,
                       const std::vector<std::pair<const llvm::Value *, int64_t>>
                           &terms);
  static value_type makeAffineCongruenceAssignment(
      const llvm::Value *dest, unsigned modulusBits, int64_t constant,
      const std::vector<std::pair<const llvm::Value *, int64_t>> &terms);

private:
  static AffineRelationVocabulary Vocabulary;
  static bool HasVocabulary;
};

} // namespace npa

#endif // NPA_AFFINE_RELATION_DOMAIN_H
