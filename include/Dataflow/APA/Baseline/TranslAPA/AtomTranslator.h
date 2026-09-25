#pragma once

// TranslAPA baseline — mechanical Gen/Kill extraction (paper §3, §4.2).
//
// The paper's translation is "mechanical": it never looks at what a dataflow
// fact *means*, only at the behavior of the IDA abstract transformer over the
// finite fact universe D. This translator realizes that claim generically by
// PROBING an existing framework transfer function.
//
// For a separable (Gen/Kill) transfer f(x) = Gen | (x \ Kill):
//   * Gen  = f({})                         -- facts generated unconditionally
//   * fact d is killed  iff  d not in f({d})
// so a per-atom (Gen, Kill) pair is recovered with |D|+1 probes of the
// client's own applyTransfer. No per-client hand translation is required; the
// client only supplies (1) the finite fact universe and (2) its applyTransfer.
//
// This class is intentionally decoupled from IntraEliminationProblem: it takes
// the fact universe and a plain apply callable, so the same code serves both
// the real solver Driver and lightweight unit tests. The fact type is any
// set-like container (default ctor, insert(v), count(v), value_type) — e.g.
// std::set<int> or std::set<const llvm::Value*>.

#include "Dataflow/APA/Baseline/TranslAPA/GenKillSemiring.h"

#include "llvm/ADT/BitVector.h"

#include <cstdint>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace elimination {
namespace translapa {

// Element type of a set-like fact container. Prefers the container's own
// value_type (e.g. std::set), and falls back to its iterator's value_type for
// bitmap-backed containers that only expose the element type on the iterator
// (e.g. the upstream IndexedSet, whose class scope has no value_type).
namespace detail {
template <typename C, typename = void> struct FactElementType {
  using type = typename C::const_iterator::value_type;
};
template <typename C>
struct FactElementType<C, std::void_t<typename C::value_type>> {
  using type = typename C::value_type;
};
} // namespace detail

// Mechanical (Gen, Kill) extractor for a single transfer atom type.
//
//   TransferT : the atom type carried by PathExprFactory (e.g. int, Instruction*)
//   FactT     : set-like fact container; its element type is deduced via
//               detail::FactElementType (value_type, or iterator value_type)
template <typename TransferT, typename FactT> class GenKillAtomTranslator {
public:
  using FactVal = typename detail::FactElementType<FactT>::type;
  using ApplyFn = std::function<FactT(const TransferT &, const FactT &)>;

  // Universe: the finite, ordered set of dataflow facts. Bit i in every
  // produced element corresponds to Universe[i].
  //
  // Separable: when the client transfer is a genuine Gen/Kill (separable)
  // function, extraction uses the O(|U|) fast path (2 probes/atom) instead of
  // the general O(|U|^2) singleton probing. See extract() for the derivation.
  GenKillAtomTranslator(std::vector<FactVal> Universe, ApplyFn Apply,
                        bool Separable = false)
      : Universe(std::move(Universe)), Apply(std::move(Apply)),
        Separable(Separable) {
    Index.reserve(this->Universe.size());
    for (std::uint32_t I = 0; I < this->Universe.size(); ++I) {
      Index.emplace(this->Universe[I], I);
    }
    if (Separable) {
      for (const auto &V : this->Universe) {
        AllFacts.insert(V);
      }
    }
  }

  unsigned universeSize() const {
    return static_cast<unsigned>(Universe.size());
  }

  // Translate one atom to its (Gen, Kill) element, caching by transfer value
  // when TransferT is hashable/comparable (the common case: pointers/ints).
  GenKillFact translate(const TransferT &T) const {
    if constexpr (isCacheable<TransferT>()) {
      auto It = Cache.find(T);
      if (It != Cache.end()) {
        return It->second;
      }
      GenKillFact F = extract(T);
      Cache.emplace(T, F);
      return F;
    } else {
      return extract(T);
    }
  }

  // Map a client fact set into the universe bit space (used for the initial
  // fact and for differential comparison).
  llvm::BitVector toBits(const FactT &S) const {
    llvm::BitVector B(universeSize(), false);
    for (const auto &V : S) {
      auto It = Index.find(V);
      if (It != Index.end()) {
        B.set(It->second);
      }
    }
    return B;
  }

  // Map a universe bit set back to a client fact set.
  FactT fromBits(const llvm::BitVector &B) const {
    FactT S;
    for (int I = B.find_first(); I != -1; I = B.find_next(I)) {
      S.insert(Universe[static_cast<std::size_t>(I)]);
    }
    return S;
  }

  const std::vector<FactVal> &universe() const { return Universe; }

private:
  template <typename T> static constexpr bool isCacheable() {
    return std::is_pointer<T>::value || std::is_integral<T>::value;
  }

  GenKillFact extract(const TransferT &T) const {
    const unsigned N = universeSize();
    llvm::BitVector Gen(N, false);
    llvm::BitVector Kill(N, false);

    // Gen = f({}).
    FactT GenSet = Apply(T, FactT{});
    for (const auto &V : GenSet) {
      auto It = Index.find(V);
      if (It != Index.end()) {
        Gen.set(It->second);
      }
    }

    if (Separable) {
      // Fast path for separable f(x) = Gen | (x \ K). Then f(U) = Gen | (U\K),
      // so U \ f(U) = (U\Gen) & K = K\Gen, and f(x) = Gen | (x \ (K\Gen)) still
      // holds. Thus Kill := U \ f(U) is a valid representation using 2 probes.
      FactT Out = Apply(T, AllFacts);
      for (std::uint32_t I = 0; I < N; ++I) {
        if (Out.count(Universe[I]) == 0) {
          Kill.set(I);
        }
      }
      return {std::move(Gen), std::move(Kill)};
    }

    // General path: fact d is killed iff d not in f({d}). O(|U|) probes.
    for (std::uint32_t I = 0; I < N; ++I) {
      FactT Single;
      Single.insert(Universe[I]);
      FactT Out = Apply(T, Single);
      if (Out.count(Universe[I]) == 0) {
        Kill.set(I);
      }
    }
    return {std::move(Gen), std::move(Kill)};
  }

  std::vector<FactVal> Universe;
  std::unordered_map<FactVal, std::uint32_t> Index;
  ApplyFn Apply;
  bool Separable = false;
  FactT AllFacts;
  mutable std::unordered_map<TransferT, GenKillFact> Cache;
};

} // namespace translapa
} // namespace elimination

