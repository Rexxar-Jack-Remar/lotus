#ifndef DATAFLOW_APA_CORE_PATHEXPR_H_
#define DATAFLOW_APA_CORE_PATHEXPR_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace elimination {

template <typename TransferT> class PathExprFactory {
public:
  enum class Kind { Zero, One, Atom, Union, Concat, Star };

  struct Expr final {
    explicit Expr(Kind K) : K(K) {}
    Expr(Kind K, TransferT Transfer)
        : K(K), Transfer(std::make_shared<TransferT>(std::move(Transfer))) {}
    Expr(Kind K, std::shared_ptr<const Expr> L, std::shared_ptr<const Expr> R)
        : K(K), L(std::move(L)), R(std::move(R)) {}
    Expr(Kind K, std::shared_ptr<const Expr> Sub) : K(K), L(std::move(Sub)) {}

    Kind K;
    std::shared_ptr<const TransferT> Transfer;
    std::shared_ptr<const Expr> L;
    std::shared_ptr<const Expr> R;
  };

  using Ref = std::shared_ptr<const Expr>;

  // Dynamic allocations only: static Zero/One singleton nodes are excluded.
  std::size_t allocationCount() const {
    return Atoms.size() + AtomIndex.size() + Unions.size() + Concats.size() +
           Stars.size();
  }
  std::size_t starAllocationCount() const { return Stars.size(); }

  // Metadata lives beside immutable nodes, keyed by root and cap. Construction
  // warms this cache; order scoring only reads it and never traverses the DAG.
  std::optional<std::size_t>
  cachedReachableNodeCount(const Ref &E, std::size_t Cap = 0) const {
    auto It = Sizes.find(E.get());
    if (It == Sizes.end()) {
      return std::nullopt;
    }
    auto Count = It->second.Counts.find(Cap);
    return Count == It->second.Counts.end()
               ? std::nullopt
               : std::optional<std::size_t>(Count->second);
  }

  std::size_t cacheReachableNodeCount(const Ref &E, std::size_t Cap = 0) const {
    if (auto Count = cachedReachableNodeCount(E, Cap)) {
      return *Count;
    }
    std::unordered_set<const Expr *> Seen;
    std::vector<const Expr *> Pending;
    if (E && !isZero(E)) {
      Pending.push_back(E.get());
    }
    while (!Pending.empty() && (Cap == 0 || Seen.size() < Cap)) {
      const auto *Node = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(Node).second) {
        continue;
      }
      if (Node->L) {
        Pending.push_back(Node->L.get());
      }
      if (Node->R) {
        Pending.push_back(Node->R.get());
      }
    }
    auto &Entry = Sizes[E.get()];
    Entry.Root =
        E; // Prevent address reuse for roots imported from other factories.
    Entry.Counts[Cap] = Seen.size();
    return Seen.size();
  }

  Ref zero() const {
    static const Ref Zero = std::make_shared<Expr>(Kind::Zero);
    return Zero;
  }

  Ref one() const {
    static const Ref One = std::make_shared<Expr>(Kind::One);
    return One;
  }

  Ref atom(TransferT T) const {
    if constexpr (is_equality_comparable<TransferT>::value &&
                  is_std_hashable<TransferT>::value) {
      // Hash-consed dedup: O(1) amortized instead of the O(atoms) linear scan
      // below. Returns the same unique node the scan would (atoms are unique by
      // value), so the exported DAG is byte-identical — only faster. Enabled
      // whenever TransferT is both hashable and comparable (e.g. Instruction*
      // intraprocedurally, and the interprocedural summary atom).
      const std::size_t H = std::hash<TransferT>{}(T);
      const auto Range = AtomIndex.equal_range(H);
      for (auto It = Range.first; It != Range.second; ++It) {
        if (*It->second->Transfer == T) {
          return It->second;
        }
      }
      auto Node = std::make_shared<Expr>(Kind::Atom, std::move(T));
      AtomIndex.emplace(H, Node);
      return Node;
    } else if constexpr (is_equality_comparable<TransferT>::value) {
      for (const auto &Existing : Atoms) {
        if (*Existing->Transfer == T) {
          return Existing;
        }
      }
      auto Node = std::make_shared<Expr>(Kind::Atom, std::move(T));
      Atoms.push_back(Node);
      return Node;
    } else {
      // Not comparable: cannot dedup, mint a fresh node each call.
      auto Node = std::make_shared<Expr>(Kind::Atom, std::move(T));
      Atoms.push_back(Node);
      return Node;
    }
  }

  Ref unite(const Ref &A, const Ref &B) const {
    if (isZero(A)) {
      return B;
    }
    if (isZero(B)) {
      return A;
    }
    if (A == B) {
      return A;
    }
    const BinaryKey Key{A.get(), B.get()};
    auto It = Unions.find(Key);
    if (It != Unions.end()) {
      return It->second;
    }
    auto Node = std::make_shared<Expr>(Kind::Union, A, B);
    Unions.emplace(Key, Node);
    return Node;
  }

  Ref concat(const Ref &A, const Ref &B) const {
    if (isZero(A) || isZero(B)) {
      return zero();
    }
    if (isOne(A)) {
      return B;
    }
    if (isOne(B)) {
      return A;
    }
    const BinaryKey Key{A.get(), B.get()};
    auto It = Concats.find(Key);
    if (It != Concats.end()) {
      return It->second;
    }
    auto Node = std::make_shared<Expr>(Kind::Concat, A, B);
    Concats.emplace(Key, Node);
    return Node;
  }

  Ref star(const Ref &A) const {
    if (isZero(A) || isOne(A)) {
      return one();
    }
    if (A->K == Kind::Star) {
      return A;
    }
    const auto *Key = A.get();
    auto It = Stars.find(Key);
    if (It != Stars.end()) {
      return It->second;
    }
    auto Node = std::make_shared<Expr>(Kind::Star, A);
    Stars.emplace(Key, Node);
    return Node;
  }

  static bool isZero(const Ref &E) { return E && E->K == Kind::Zero; }
  static bool isOne(const Ref &E) { return E && E->K == Kind::One; }

private:
  template <typename T, typename = void>
  struct is_equality_comparable : std::false_type {};

  template <typename T>
  struct is_equality_comparable<
      T, std::void_t<decltype(std::declval<const T &>() ==
                              std::declval<const T &>())>> : std::true_type {};

  template <typename T, typename = void>
  struct is_std_hashable : std::false_type {};

  template <typename T>
  struct is_std_hashable<
      T, std::void_t<decltype(std::declval<std::hash<T>>()(
             std::declval<const T &>()))>> : std::true_type {};

  struct BinaryKey final {
    const Expr *L = nullptr;
    const Expr *R = nullptr;

    bool operator==(const BinaryKey &Other) const {
      return L == Other.L && R == Other.R;
    }
  };

  struct BinaryKeyHash final {
    std::size_t operator()(const BinaryKey &Key) const {
      const auto LH = std::hash<const Expr *>{}(Key.L);
      const auto RH = std::hash<const Expr *>{}(Key.R);
      return hashCombine(LH, RH);
    }
  };

  static std::size_t hashCombine(std::size_t L, std::size_t R) {
    return L ^ (R + 0x9e3779b97f4a7c15ULL + (L << 6) + (L >> 2));
  }

  mutable std::vector<Ref> Atoms;
  // Hash index over atom Transfer values (used when TransferT is hashable);
  // buckets map hash(Transfer) -> atom node for O(1) amortized hash-consing.
  mutable std::unordered_multimap<std::size_t, Ref> AtomIndex;
  mutable std::unordered_map<BinaryKey, Ref, BinaryKeyHash> Unions;
  mutable std::unordered_map<BinaryKey, Ref, BinaryKeyHash> Concats;
  mutable std::unordered_map<const Expr *, Ref> Stars;
  struct SizeEntry final {
    Ref Root;
    std::unordered_map<std::size_t, std::size_t> Counts;
  };
  mutable std::unordered_map<const Expr *, SizeEntry> Sizes;
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_PATHEXPR_H_
