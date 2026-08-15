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
using Binding = std::vector<std::optional<std::any>>;

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

struct ReducerIR {
  std::function<std::any()> make_state;
  std::function<void(std::any &, const std::any &)> add;
  std::function<void(std::any &, const std::any &)> merge;
  std::function<std::vector<std::any>(std::any &)> finish;
};

struct AggregateIR {
  VarId output_var = 0;
  std::type_index output_type = typeid(void);
  AtomIR source;
  ExprIR projection;
  std::string name;
  std::function<std::vector<std::any>(const std::vector<std::any> &)>
      evaluate_range;
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
