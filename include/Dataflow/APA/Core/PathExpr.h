#ifndef DATAFLOW_APA_CORE_PATHEXPR_H_
#define DATAFLOW_APA_CORE_PATHEXPR_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>
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

  Ref zero() const {
    static const Ref Zero = std::make_shared<Expr>(Kind::Zero);
    return Zero;
  }

  Ref one() const {
    static const Ref One = std::make_shared<Expr>(Kind::One);
    return One;
  }

  Ref atom(TransferT T) const {
    if constexpr (is_equality_comparable<TransferT>::value) {
      for (const auto &Existing : Atoms) {
        if (*Existing->Transfer == T) {
          return Existing;
        }
      }
    }
    auto Node = std::make_shared<Expr>(Kind::Atom, std::move(T));
    Atoms.push_back(Node);
    return Node;
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
  mutable std::unordered_map<BinaryKey, Ref, BinaryKeyHash> Unions;
  mutable std::unordered_map<BinaryKey, Ref, BinaryKeyHash> Concats;
  mutable std::unordered_map<const Expr *, Ref> Stars;
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_PATHEXPR_H_
