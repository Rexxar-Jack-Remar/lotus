#pragma once

// PathLang: the e-graph-side representation of APA path expressions.
//
// This is a TYPED language (egg `define_language!` style): a path-expression
// e-node is one of a fixed set of kinds, and an atom carries its transfer id as
// a typed integer payload rather than being encoded into an operator STRING
// ("atom#<id>") as in the previous SymbolLang-based representation. This removes
// per-node string allocation/parsing and, crucially, orders atoms NUMERICALLY
// (atom 2 < atom 10) instead of lexicographically ("atom#10" < "atom#2"), which
// the extractor's tie-break relies on.
//
// Operator kinds (see docs/EAN_项目计划书.md, M1):
//   Zero  leaf     -- 0  (no path)
//   One   leaf     -- 1  (empty path)
//   Atom  leaf     -- opaque transfer atom, id keyed by AtomTable
//   Join  variadic -- ⊕  (choice);   children sorted + deduped (ACI)
//   Seq   variadic -- ·  (sequence);  children order-preserving
//   Star  arity 1  -- *  (iteration)
//
// The variadic-vs-canonical rules (flatten / sort / dedup for join, flatten /
// keep-order for seq) are enforced by Import/Canonical, not here; this header
// only defines the node type, its Language interface, and named constructors.
//
// Downstream code (Canonical/Factorize/Star/Import/Export/AtomTable) uses only
// the is*/make*/parseAtomId helpers below, so it is agnostic to this typed
// representation.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Solvers/EGraph/Id.h"
#include "Solvers/EGraph/Language.h"
#include "Solvers/EGraph/Util.h"

namespace elimination {
namespace ean {

using Id = ::lotus::egraph::Id;

enum class PathKind : std::uint8_t { Zero, One, Atom, Seq, Join, Star };

struct PathDiscriminant {
  PathKind kind = PathKind::Zero;
  std::uint32_t atom = 0;  // meaningful only when kind == Atom
  std::uint32_t arity = 0;

  friend bool operator==(const PathDiscriminant &lhs,
                         const PathDiscriminant &rhs) {
    return lhs.kind == rhs.kind && lhs.atom == rhs.atom &&
           lhs.arity == rhs.arity;
  }
  friend bool operator!=(const PathDiscriminant &lhs,
                         const PathDiscriminant &rhs) {
    return !(lhs == rhs);
  }
};

// A path-expression e-node. Satisfies the lotus::egraph Language concept.
class PathNode {
public:
  using Discriminant = PathDiscriminant;

  PathNode() = default;

  // Named constructors (mirror the previous make* free functions).
  static PathNode zero() { return PathNode(PathKind::Zero, 0, {}); }
  static PathNode one() { return PathNode(PathKind::One, 0, {}); }
  static PathNode atom(std::uint32_t id) { return PathNode(PathKind::Atom, id, {}); }
  static PathNode seq(std::vector<Id> children) {
    return PathNode(PathKind::Seq, 0, std::move(children));
  }
  static PathNode join(std::vector<Id> children) {
    return PathNode(PathKind::Join, 0, std::move(children));
  }
  static PathNode star(Id body) {
    return PathNode(PathKind::Star, 0, std::vector<Id>{body});
  }

  PathKind kind() const { return kind_; }
  std::uint32_t atomId() const { return atom_; }

  const std::vector<Id> &children() const { return children_; }
  std::vector<Id> &childrenMut() { return children_; }

  Discriminant discriminant() const {
    return Discriminant{kind_, atom_,
                        static_cast<std::uint32_t>(children_.size())};
  }

  bool matches(const PathNode &other) const {
    return kind_ == other.kind_ && atom_ == other.atom_ &&
           children_.size() == other.children_.size();
  }

  template <typename F> void forEach(F &&fn) const {
    for (Id id : children_) {
      fn(id);
    }
  }
  template <typename F> void forEachMut(F &&fn) {
    for (Id &id : children_) {
      fn(id);
    }
  }

  template <typename F> PathNode mapChildren(F &&fn) const {
    PathNode copy = *this;
    for (Id &id : copy.children_) {
      id = fn(id);
    }
    return copy;
  }

  bool isLeaf() const { return children_.empty(); }

  friend bool operator==(const PathNode &lhs, const PathNode &rhs) {
    return lhs.kind_ == rhs.kind_ && lhs.atom_ == rhs.atom_ &&
           lhs.children_ == rhs.children_;
  }
  friend bool operator!=(const PathNode &lhs, const PathNode &rhs) {
    return !(lhs == rhs);
  }
  // Numeric atom ordering (atom 2 < atom 10), unlike the old string order.
  friend bool operator<(const PathNode &lhs, const PathNode &rhs) {
    return std::tie(lhs.kind_, lhs.atom_, lhs.children_) <
           std::tie(rhs.kind_, rhs.atom_, rhs.children_);
  }

private:
  PathNode(PathKind kind, std::uint32_t atom, std::vector<Id> children)
      : kind_(kind), atom_(atom), children_(std::move(children)) {}

  PathKind kind_ = PathKind::Zero;
  std::uint32_t atom_ = 0;
  std::vector<Id> children_;
};

// The e-graph language EAN operates on.
using PathLang = PathNode;

// ---- node kind classification ----------------------------------------------

inline bool isZero(const PathLang &n) { return n.kind() == PathKind::Zero; }
inline bool isOne(const PathLang &n) { return n.kind() == PathKind::One; }
inline bool isJoin(const PathLang &n) { return n.kind() == PathKind::Join; }
inline bool isSeq(const PathLang &n) { return n.kind() == PathKind::Seq; }
inline bool isStar(const PathLang &n) { return n.kind() == PathKind::Star; }
inline bool isAtom(const PathLang &n) { return n.kind() == PathKind::Atom; }

// Precondition: isAtom(n) is true.
inline std::uint32_t parseAtomId(const PathLang &n) { return n.atomId(); }

// ---- node builders ----------------------------------------------------------

inline PathLang makeZero() { return PathLang::zero(); }
inline PathLang makeOne() { return PathLang::one(); }
inline PathLang makeAtom(std::uint32_t id) { return PathLang::atom(id); }
inline PathLang makeJoin(std::vector<Id> children) {
  return PathLang::join(std::move(children));
}
inline PathLang makeSeq(std::vector<Id> children) {
  return PathLang::seq(std::move(children));
}
inline PathLang makeStar(Id body) { return PathLang::star(body); }

} // namespace ean
} // namespace elimination

// ---- Language concept: node display (the e-graph instantiates displayNode<L>
// for its dot/debug path). JSON decode (LanguageOps::fromOp) is not used by EAN,
// so only the display half is provided. --------------------------------------

namespace lotus::egraph {

template <> struct LanguageOps<::elimination::ean::PathNode> {
  static std::string display(const ::elimination::ean::PathNode &node) {
    using Kind = ::elimination::ean::PathKind;
    switch (node.kind()) {
    case Kind::Zero:
      return "zero";
    case Kind::One:
      return "one";
    case Kind::Atom:
      return "atom#" + std::to_string(node.atomId());
    case Kind::Seq:
      return "seq";
    case Kind::Join:
      return "join";
    case Kind::Star:
      return "star";
    }
    return "zero";
  }
};

} // namespace lotus::egraph

// ---- hashing (required by the e-graph's unordered_map memo/index) -----------

template <> struct std::hash<::elimination::ean::PathDiscriminant> {
  size_t operator()(
      const ::elimination::ean::PathDiscriminant &value) const noexcept {
    size_t seed = static_cast<size_t>(value.kind);
    ::lotus::egraph::hashCombine(seed, value.atom);
    ::lotus::egraph::hashCombine(seed, value.arity);
    return seed;
  }
};

template <> struct std::hash<::elimination::ean::PathNode> {
  size_t operator()(const ::elimination::ean::PathNode &value) const noexcept {
    size_t seed = static_cast<size_t>(value.kind());
    ::lotus::egraph::hashCombine(seed, value.atomId());
    for (::lotus::egraph::Id child : value.children()) {
      ::lotus::egraph::hashCombine(seed, child);
    }
    return seed;
  }
};

