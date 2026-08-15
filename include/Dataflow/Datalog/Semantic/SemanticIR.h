#pragma once

#include <any>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <typeindex>
#include <variant>
#include <vector>

namespace lotus::datalog {

using RelationId = std::size_t;
using VarId = std::size_t;
using ColumnMask = std::size_t;

// Body scans bind variables to immutable relation cells. Keeping a reference
// here avoids copying a std::any (and often its heap allocation) at every join
// step. Computed values, such as aggregate outputs, are owned by the slot
// instead.
class BindingSlot {
public:
  BindingSlot() = default;

  BindingSlot(const BindingSlot &other) { copyFrom(other); }
  BindingSlot &operator=(const BindingSlot &other) {
    if (this != &other)
      copyFrom(other);
    return *this;
  }

  BindingSlot(BindingSlot &&other) noexcept { moveFrom(std::move(other)); }
  BindingSlot &operator=(BindingSlot &&other) noexcept {
    if (this != &other)
      moveFrom(std::move(other));
    return *this;
  }

  BindingSlot &operator=(const std::any &value) {
    bindOwned(value);
    return *this;
  }

  BindingSlot &operator=(std::any &&value) {
    bindOwned(std::move(value));
    return *this;
  }

  explicit operator bool() const { return value_ != nullptr; }
  const std::any &operator*() const { return *value_; }

  void bindReference(const std::any &value) {
    owned_.reset();
    value_ = &value;
  }

  void bindOwned(std::any value) {
    owned_.emplace(std::move(value));
    value_ = &*owned_;
  }

  void reset() {
    owned_.reset();
    value_ = nullptr;
  }

  bool ownsValue() const { return owned_.has_value(); }

private:
  void copyFrom(const BindingSlot &other) {
    if (other.owned_) {
      owned_ = other.owned_;
      value_ = &*owned_;
    } else {
      owned_.reset();
      value_ = other.value_;
    }
  }

  void moveFrom(BindingSlot &&other) {
    if (other.owned_) {
      owned_ = std::move(other.owned_);
      value_ = &*owned_;
    } else {
      owned_.reset();
      value_ = other.value_;
    }
    other.reset();
  }

  std::optional<std::any> owned_;
  const std::any *value_ = nullptr;
};

using Binding = std::vector<BindingSlot>;

enum class DependencyKind {
  Positive,
  Negative,
  Aggregate,
};

enum class RelationKind {
  Set,
  Lattice,
};

struct ColumnType {
  std::type_index type = typeid(void);
  std::string name;
  std::function<std::size_t(const std::any &)> hash;
  std::function<bool(const std::any &, const std::any &)> equal;
  std::function<void(const std::any &)> validate;
  std::function<void(const std::any &)> validate_key;
};

struct ExprIR {
  std::type_index type = typeid(void);
  std::vector<VarId> referenced_vars;
  std::function<std::any(const Binding &)> evaluate;
  std::string debug_name;
};

struct TermIR {
  enum class Kind {
    Variable,
    Constant,
    Expression,
  };

  Kind kind = Kind::Constant;
  std::type_index type = typeid(void);
  VarId variable = 0;
  bool anonymous = false;
  std::any constant;
  ExprIR expression;
  std::string debug_name;
};

struct AtomIR {
  RelationId relation = 0;
  std::string relation_name;
  std::vector<TermIR> args;
};

struct FilterIR {
  ExprIR predicate;
};

struct NegAtomIR {
  AtomIR atom;
};

// User reducers are serial by default.  Parallel evaluation is enabled only
// when callers explicitly attest that partitioned add/merge is valid and that
// execution order cannot affect the observable result.
struct ReducerProperties {
  bool associative = false;
  bool commutative = false;
  bool deterministic = false;
  bool parallel_safe = false;

  static constexpr ReducerProperties parallel() {
    return {true, true, true, true};
  }

  constexpr bool canRunInParallel() const {
    return associative && commutative && deterministic && parallel_safe;
  }
};

struct ReducerIR {
  std::function<std::any()> make_state;
  std::function<void(std::any &, const std::any &)> add;
  std::function<void(std::any &, const std::any &)> merge;
  std::function<std::vector<std::any>(std::any &)> finish;
  ReducerProperties properties;
};

using AggregateConsumer = std::function<void(const std::any &)>;
using AggregateForEach = std::function<void(const AggregateConsumer &consumer)>;

struct AggregateIR {
  VarId output_var = 0;
  std::type_index output_type = typeid(void);
  AtomIR source;
  ExprIR projection;
  std::string name;
  std::function<std::vector<std::any>(const AggregateForEach &)> evaluate;
  std::optional<ReducerIR> reducer;
};

using BodyItemIR = std::variant<AtomIR, FilterIR, NegAtomIR, AggregateIR>;

struct RuleIR {
  AtomIR head;
  std::vector<BodyItemIR> body;
};

struct RelationIR {
  RelationId id = 0;
  std::string name;
  std::vector<ColumnType> columns;
  RelationKind kind = RelationKind::Set;
  std::function<bool(std::any &, const std::any &)> lattice_join;
};

} // namespace lotus::datalog
