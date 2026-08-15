#include "JsonFrontend.h"

#include "Dataflow/Datalog/Datalog.h"

#include <algorithm>
#include <any>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/Optional.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>

namespace lotus::datalog::cli {
namespace {

using llvm::StringRef;
using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;

enum class ValueKind {
  I64,
  U64,
  F64,
  String,
  Bool,
  MinI64,
  MaxI64,
  MinF64,
  MaxF64,
  SetI64,
};

struct TypeSpec {
  ValueKind kind = ValueKind::I64;

  bool operator==(const TypeSpec &other) const { return kind == other.kind; }
  bool operator!=(const TypeSpec &other) const { return !(*this == other); }

  bool isLattice() const {
    return kind == ValueKind::MinI64 || kind == ValueKind::MaxI64 ||
           kind == ValueKind::MinF64 || kind == ValueKind::MaxF64 ||
           kind == ValueKind::SetI64;
  }

  std::string name() const {
    switch (kind) {
    case ValueKind::I64:
      return "i64";
    case ValueKind::U64:
      return "u64";
    case ValueKind::F64:
      return "f64";
    case ValueKind::String:
      return "string";
    case ValueKind::Bool:
      return "bool";
    case ValueKind::MinI64:
      return "min<i64>";
    case ValueKind::MaxI64:
      return "max<i64>";
    case ValueKind::MinF64:
      return "min<f64>";
    case ValueKind::MaxF64:
      return "max<f64>";
    case ValueKind::SetI64:
      return "set<i64>";
    }
    throw std::logic_error("unknown JSON Datalog type");
  }

  std::type_index typeIndex() const {
    switch (kind) {
    case ValueKind::I64:
      return typeid(std::int64_t);
    case ValueKind::U64:
      return typeid(std::uint64_t);
    case ValueKind::F64:
      return typeid(double);
    case ValueKind::String:
      return typeid(std::string);
    case ValueKind::Bool:
      return typeid(bool);
    case ValueKind::MinI64:
      return typeid(MinLattice<std::int64_t>);
    case ValueKind::MaxI64:
      return typeid(MaxLattice<std::int64_t>);
    case ValueKind::MinF64:
      return typeid(MinLattice<double>);
    case ValueKind::MaxF64:
      return typeid(MaxLattice<double>);
    case ValueKind::SetI64:
      return typeid(SetLattice<std::int64_t>);
    }
    throw std::logic_error("unknown JSON Datalog type");
  }

  ColumnType columnType() const {
    switch (kind) {
    case ValueKind::I64:
      return detail::makeColumnType<std::int64_t>();
    case ValueKind::U64:
      return detail::makeColumnType<std::uint64_t>();
    case ValueKind::F64:
      return detail::makeColumnType<double>();
    case ValueKind::String:
      return detail::makeColumnType<std::string>();
    case ValueKind::Bool:
      return detail::makeColumnType<bool>();
    case ValueKind::MinI64:
      return detail::makeColumnType<MinLattice<std::int64_t>>();
    case ValueKind::MaxI64:
      return detail::makeColumnType<MaxLattice<std::int64_t>>();
    case ValueKind::MinF64:
      return detail::makeColumnType<MinLattice<double>>();
    case ValueKind::MaxF64:
      return detail::makeColumnType<MaxLattice<double>>();
    case ValueKind::SetI64:
      return detail::makeColumnType<SetLattice<std::int64_t>>();
    }
    throw std::logic_error("unknown JSON Datalog type");
  }
};

TypeSpec parseType(StringRef name) {
  if (name == "i64")
    return {ValueKind::I64};
  if (name == "u64")
    return {ValueKind::U64};
  if (name == "f64")
    return {ValueKind::F64};
  if (name == "string")
    return {ValueKind::String};
  if (name == "bool")
    return {ValueKind::Bool};
  if (name == "min<i64>")
    return {ValueKind::MinI64};
  if (name == "max<i64>")
    return {ValueKind::MaxI64};
  if (name == "min<f64>")
    return {ValueKind::MinF64};
  if (name == "max<f64>")
    return {ValueKind::MaxF64};
  if (name == "set<i64>")
    return {ValueKind::SetI64};
  throw std::invalid_argument("unknown column type '" + name.str() + "'");
}

std::int64_t requireI64(const Value &value, StringRef field) {
  llvm::Optional<std::int64_t> integer = value.getAsInteger();
  if (!integer)
    throw std::invalid_argument(field.str() + " must be an integer");
  return *integer;
}

double requireF64(const Value &value, StringRef field) {
  llvm::Optional<double> number = value.getAsNumber();
  if (!number)
    throw std::invalid_argument(field.str() + " must be a number");
  return *number;
}

std::any parseConstant(const Value &value, TypeSpec type, StringRef field) {
  switch (type.kind) {
  case ValueKind::I64:
    return requireI64(value, field);
  case ValueKind::U64: {
    const std::int64_t integer = requireI64(value, field);
    if (integer < 0)
      throw std::invalid_argument(field.str() + " must be non-negative");
    return static_cast<std::uint64_t>(integer);
  }
  case ValueKind::F64:
    return requireF64(value, field);
  case ValueKind::String: {
    llvm::Optional<StringRef> string = value.getAsString();
    if (!string)
      throw std::invalid_argument(field.str() + " must be a string");
    return string->str();
  }
  case ValueKind::Bool: {
    llvm::Optional<bool> boolean = value.getAsBoolean();
    if (!boolean)
      throw std::invalid_argument(field.str() + " must be a boolean");
    return *boolean;
  }
  case ValueKind::MinI64:
    return MinLattice<std::int64_t>(requireI64(value, field));
  case ValueKind::MaxI64:
    return MaxLattice<std::int64_t>(requireI64(value, field));
  case ValueKind::MinF64:
    return MinLattice<double>(requireF64(value, field));
  case ValueKind::MaxF64:
    return MaxLattice<double>(requireF64(value, field));
  case ValueKind::SetI64: {
    const Array *array = value.getAsArray();
    if (!array)
      throw std::invalid_argument(field.str() + " must be an integer array");
    std::set<std::int64_t> values;
    for (const Value &element : *array)
      values.insert(requireI64(element, field));
    return SetLattice<std::int64_t>(std::move(values));
  }
  }
  throw std::logic_error("unknown JSON Datalog type");
}

Value encodeValue(const std::any &value, TypeSpec type) {
  switch (type.kind) {
  case ValueKind::I64:
    return std::any_cast<const std::int64_t &>(value);
  case ValueKind::U64: {
    const std::uint64_t integer = std::any_cast<const std::uint64_t &>(value);
    if (integer >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return std::to_string(integer);
    return static_cast<std::int64_t>(integer);
  }
  case ValueKind::F64:
    return std::any_cast<const double &>(value);
  case ValueKind::String:
    return std::any_cast<const std::string &>(value);
  case ValueKind::Bool:
    return std::any_cast<const bool &>(value);
  case ValueKind::MinI64:
    return std::any_cast<const MinLattice<std::int64_t> &>(value).value();
  case ValueKind::MaxI64:
    return std::any_cast<const MaxLattice<std::int64_t> &>(value).value();
  case ValueKind::MinF64:
    return std::any_cast<const MinLattice<double> &>(value).value();
  case ValueKind::MaxF64:
    return std::any_cast<const MaxLattice<double> &>(value).value();
  case ValueKind::SetI64: {
    Array result;
    for (std::int64_t element :
         std::any_cast<const SetLattice<std::int64_t> &>(value).values())
      result.push_back(element);
    return result;
  }
  }
  throw std::logic_error("unknown JSON Datalog type");
}

std::function<bool(std::any &, const std::any &)> latticeJoin(TypeSpec type) {
  switch (type.kind) {
  case ValueKind::MinI64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<MinLattice<std::int64_t> &>(current).joinMut(
          std::any_cast<const MinLattice<std::int64_t> &>(candidate));
    };
  case ValueKind::MaxI64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<MaxLattice<std::int64_t> &>(current).joinMut(
          std::any_cast<const MaxLattice<std::int64_t> &>(candidate));
    };
  case ValueKind::MinF64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<MinLattice<double> &>(current).joinMut(
          std::any_cast<const MinLattice<double> &>(candidate));
    };
  case ValueKind::MaxF64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<MaxLattice<double> &>(current).joinMut(
          std::any_cast<const MaxLattice<double> &>(candidate));
    };
  case ValueKind::SetI64:
    return [](std::any &current, const std::any &candidate) {
      return std::any_cast<SetLattice<std::int64_t> &>(current).joinMut(
          std::any_cast<const SetLattice<std::int64_t> &>(candidate));
    };
  default:
    throw std::invalid_argument("lattice value type must be min, max, or set");
  }
}

StringRef requireString(const Object &object, StringRef key) {
  llvm::Optional<StringRef> value = object.getString(key);
  if (!value || value->empty())
    throw std::invalid_argument("missing string field '" + key.str() + "'");
  return *value;
}

const Object &requireObject(const Value &value, StringRef field) {
  const Object *object = value.getAsObject();
  if (!object)
    throw std::invalid_argument(field.str() + " must be an object");
  return *object;
}

const Array &requireArray(const Object &object, StringRef key) {
  const Array *array = object.getArray(key);
  if (!array)
    throw std::invalid_argument("missing array field '" + key.str() + "'");
  return *array;
}

struct RelationSpec {
  RelationId id = 0;
  std::string name;
  std::vector<TypeSpec> columns;
};

struct VariableSpec {
  VarId id = 0;
  TypeSpec type;
};

struct RuleScope {
  std::size_t rule_index = 0;
  std::unordered_map<std::string, VariableSpec> variables;
};

struct DynamicExpr {
  TypeSpec type;
  ExprIR ir;
};

template <typename T> DynamicExpr constantExpr(TypeSpec type, T value) {
  ExprIR expression;
  expression.type = type.typeIndex();
  expression.debug_name = "constant";
  expression.evaluate = [value = std::move(value)](const Binding &) {
    return std::any(value);
  };
  return {type, std::move(expression)};
}

DynamicExpr constantAnyExpr(TypeSpec type, std::any value) {
  ExprIR expression;
  expression.type = type.typeIndex();
  expression.debug_name = "constant";
  expression.evaluate = [value = std::move(value)](const Binding &) {
    return value;
  };
  return {type, std::move(expression)};
}

DynamicExpr variableExpr(const VariableSpec &variable) {
  ExprIR expression;
  expression.type = variable.type.typeIndex();
  expression.referenced_vars = {variable.id};
  expression.debug_name = "variable";
  expression.evaluate = [id = variable.id](const Binding &binding) {
    if (id >= binding.size() || !binding[id])
      throw std::logic_error("evaluating an unbound JSON Datalog variable");
    return *binding[id];
  };
  return {variable.type, std::move(expression)};
}

template <typename L, typename R, typename Result, typename Function>
DynamicExpr binaryExpr(TypeSpec result_type, DynamicExpr lhs, DynamicExpr rhs,
                       Function function, std::string name) {
  ExprIR expression;
  expression.type = result_type.typeIndex();
  expression.referenced_vars =
      detail::mergeReferences(lhs.ir.referenced_vars, rhs.ir.referenced_vars);
  expression.debug_name = std::move(name);
  expression.evaluate = [left = std::move(lhs.ir), right = std::move(rhs.ir),
                         function =
                             std::move(function)](const Binding &binding) {
    return std::any(function(std::any_cast<L>(left.evaluate(binding)),
                             std::any_cast<R>(right.evaluate(binding))));
  };
  return {result_type, std::move(expression)};
}

template <typename Input, typename Result, typename Function>
DynamicExpr unaryExpr(TypeSpec result_type, DynamicExpr operand,
                      Function function, std::string name) {
  ExprIR expression;
  expression.type = result_type.typeIndex();
  expression.referenced_vars = operand.ir.referenced_vars;
  expression.debug_name = std::move(name);
  expression.evaluate = [input = std::move(operand.ir),
                         function =
                             std::move(function)](const Binding &binding) {
    return std::any(function(std::any_cast<Input>(input.evaluate(binding))));
  };
  return {result_type, std::move(expression)};
}

template <typename T, typename Function>
DynamicExpr sameTypeBinary(TypeSpec type, DynamicExpr lhs, DynamicExpr rhs,
                           Function function, StringRef name) {
  return binaryExpr<T, T, T>(type, std::move(lhs), std::move(rhs),
                             std::move(function), name.str());
}

template <typename T, typename Function>
DynamicExpr comparison(TypeSpec operand_type, DynamicExpr lhs, DynamicExpr rhs,
                       Function function, StringRef name) {
  (void)operand_type;
  return binaryExpr<T, T, bool>({ValueKind::Bool}, std::move(lhs),
                                std::move(rhs), std::move(function),
                                name.str());
}

class JsonProgram {
public:
  explicit JsonProgram(const Object &root) { parse(root); }

  Object execute(const RunOptions &options) {
    CompiledProgram compiled = program_.compile();
    if (!options.validate_only)
      compiled.run(options.execution);

    Object result;
    result["status"] = options.validate_only ? "valid" : "ok";
    if (!options.validate_only) {
      Object relations;
      for (RelationId relation_id : outputs_) {
        const RelationSpec &relation = relations_.at(relation_id);
        std::vector<std::pair<std::string, Array>> encoded_rows;
        for (const std::vector<std::any> &row : program_.rows(relation_id)) {
          Array encoded;
          for (std::size_t column = 0; column < row.size(); ++column)
            encoded.push_back(
                encodeValue(row[column], relation.columns[column]));
          std::string key = llvm::formatv("{0}", Value(Array(encoded))).str();
          encoded_rows.emplace_back(std::move(key), std::move(encoded));
        }
        std::sort(encoded_rows.begin(), encoded_rows.end(),
                  [](const auto &lhs, const auto &rhs) {
                    return lhs.first < rhs.first;
                  });
        Array rows;
        for (auto &entry : encoded_rows)
          rows.push_back(std::move(entry.second));
        relations[relation.name] = std::move(rows);
      }
      result["relations"] = std::move(relations);
      result["stats"] = encodeStats(compiled.stats());
    }
    return result;
  }

private:
  static Object encodeStats(const ExecutionStats &stats) {
    return Object{
        {"rule_evaluations", static_cast<std::int64_t>(stats.rule_evaluations)},
        {"tuples_scanned", static_cast<std::int64_t>(stats.tuples_scanned)},
        {"index_lookups", static_cast<std::int64_t>(stats.index_lookups)},
        {"inserted_facts", static_cast<std::int64_t>(stats.inserted_facts)},
        {"fixpoint_iterations",
         static_cast<std::int64_t>(stats.fixpoint_iterations)},
        {"planned_reorders", static_cast<std::int64_t>(stats.planned_reorders)},
        {"parallel_tasks", static_cast<std::int64_t>(stats.parallel_tasks)},
        {"parallel_rule_tasks",
         static_cast<std::int64_t>(stats.parallel_rule_tasks)},
        {"parallel_merge_tasks",
         static_cast<std::int64_t>(stats.parallel_merge_tasks)},
        {"parallel_aggregate_tasks",
         static_cast<std::int64_t>(stats.parallel_aggregate_tasks)},
        {"scc_count", static_cast<std::int64_t>(stats.scc_count)},
        {"relation_count", static_cast<std::int64_t>(stats.relation_count)},
        {"total_facts", static_cast<std::int64_t>(stats.total_facts)},
        {"peak_delta", static_cast<std::int64_t>(stats.peak_delta)},
        {"index_count", static_cast<std::int64_t>(stats.index_count)},
        {"index_entries", static_cast<std::int64_t>(stats.index_entries)},
        {"index_memory_bytes",
         static_cast<std::int64_t>(stats.index_memory_bytes)}};
  }

  void parse(const Object &root) {
    const Array &relations = requireArray(root, "relations");
    for (const Value &relation : relations)
      parseRelation(requireObject(relation, "relation"));

    if (const Array *rules = root.getArray("rules")) {
      for (std::size_t index = 0; index < rules->size(); ++index)
        parseRule(requireObject((*rules)[index], "rule"), index);
    }

    if (const Array *outputs = root.getArray("outputs")) {
      for (const Value &output : *outputs) {
        llvm::Optional<StringRef> name = output.getAsString();
        if (!name)
          throw std::invalid_argument("output relation names must be strings");
        outputs_.push_back(findRelation(*name).id);
      }
    } else {
      for (const RelationSpec &relation : relations_)
        outputs_.push_back(relation.id);
    }
  }

  void parseRelation(const Object &object) {
    const std::string name = requireString(object, "name").str();
    if (relation_ids_.count(name))
      throw std::invalid_argument("duplicate relation '" + name + "'");
    const Array &column_values = requireArray(object, "columns");
    if (column_values.empty())
      throw std::invalid_argument("relation '" + name + "' has no columns");

    std::vector<TypeSpec> types;
    std::vector<ColumnType> columns;
    for (const Value &column : column_values) {
      llvm::Optional<StringRef> type_name = column.getAsString();
      if (!type_name)
        throw std::invalid_argument("relation column types must be strings");
      TypeSpec type = parseType(*type_name);
      types.push_back(type);
      columns.push_back(type.columnType());
    }

    const StringRef kind = object.getString("kind").getValueOr("relation");
    const bool is_lattice = kind == "lattice";
    if (!is_lattice && kind != "relation")
      throw std::invalid_argument("relation kind must be relation or lattice");
    if (is_lattice && !types.back().isLattice()) {
      throw std::invalid_argument("lattice relation '" + name +
                                  "' requires a lattice final column");
    }

    RelationId id = program_.addRelation(
        name, std::move(columns),
        is_lattice ? RelationKind::Lattice : RelationKind::Set,
        is_lattice ? latticeJoin(types.back())
                   : std::function<bool(std::any &, const std::any &)>{});
    relation_ids_[name] = id;
    relations_.push_back({id, name, std::move(types)});

    if (const Array *facts = object.getArray("facts")) {
      for (std::size_t row_index = 0; row_index < facts->size(); ++row_index) {
        const Array *row = (*facts)[row_index].getAsArray();
        if (!row || row->size() != relations_.back().columns.size()) {
          throw std::invalid_argument("fact arity mismatch in relation '" +
                                      name + "'");
        }
        std::vector<std::any> values;
        for (std::size_t column = 0; column < row->size(); ++column) {
          values.push_back(parseConstant(
              (*row)[column], relations_.back().columns[column], "fact"));
        }
        program_.addFact(id, std::move(values));
      }
    }
  }

  const RelationSpec &findRelation(StringRef name) const {
    auto found = relation_ids_.find(name.str());
    if (found == relation_ids_.end())
      throw std::invalid_argument("unknown relation '" + name.str() + "'");
    return relations_.at(found->second);
  }

  VariableSpec &variable(RuleScope &scope, StringRef name, TypeSpec type) {
    name.consume_front("$");
    const std::string normalized = name.str();
    if (normalized.empty())
      throw std::invalid_argument("variable name must not be empty");
    auto found = scope.variables.find(normalized);
    if (found != scope.variables.end()) {
      if (found->second.type != type) {
        throw std::invalid_argument("variable '" + normalized +
                                    "' has inconsistent types");
      }
      return found->second;
    }
    const std::string internal_name =
        "r" + std::to_string(scope.rule_index) + ":" + normalized;
    VarId id = program_.addVariable(internal_name, type.typeIndex());
    return scope.variables.emplace(normalized, VariableSpec{id, type})
        .first->second;
  }

  VariableSpec &findVariable(RuleScope &scope, StringRef name) {
    name.consume_front("$");
    auto found = scope.variables.find(name.str());
    if (found == scope.variables.end())
      throw std::invalid_argument("unknown variable '" + name.str() + "'");
    return found->second;
  }

  void registerAtomVariables(const Object &atom, RuleScope &scope) {
    const RelationSpec &relation =
        findRelation(requireString(atom, "relation"));
    const Array &args = requireArray(atom, "args");
    if (args.size() != relation.columns.size())
      throw std::invalid_argument("atom arity mismatch for '" + relation.name +
                                  "'");
    for (std::size_t column = 0; column < args.size(); ++column) {
      if (llvm::Optional<StringRef> string = args[column].getAsString()) {
        if (string->startswith("$") && string->size() > 1)
          variable(scope, *string, relation.columns[column]);
      } else if (const Object *object = args[column].getAsObject()) {
        if (llvm::Optional<StringRef> name = object->getString("var"))
          variable(scope, *name, relation.columns[column]);
      }
    }
  }

  void registerRuleVariables(const Object &rule, RuleScope &scope) {
    if (const Object *head = rule.getObject("head"))
      registerAtomVariables(*head, scope);
    if (const Array *heads = rule.getArray("heads")) {
      for (const Value &head : *heads)
        registerAtomVariables(requireObject(head, "head"), scope);
    }
    const Array &body = requireArray(rule, "body");
    for (const Value &item_value : body) {
      const Object &item = requireObject(item_value, "body item");
      if (const Object *atom = item.getObject("atom"))
        registerAtomVariables(*atom, scope);
      else if (const Object *negation = item.getObject("not"))
        registerAtomVariables(*negation, scope);
      else if (const Object *aggregate = item.getObject("aggregate")) {
        const Object *source = aggregate->getObject("source");
        if (!source)
          throw std::invalid_argument("aggregate requires a source atom");
        registerAtomVariables(*source, scope);
      }
    }
  }

  TermIR parseTerm(const Value &value, TypeSpec expected, RuleScope &scope,
                   bool allow_expression) {
    if (llvm::Optional<StringRef> string = value.getAsString()) {
      if (*string == "_") {
        VarId id = program_.addVariable("_", expected.typeIndex(), true);
        TermIR term;
        term.kind = TermIR::Kind::Variable;
        term.type = expected.typeIndex();
        term.variable = id;
        term.anonymous = true;
        term.debug_name = "_";
        return term;
      }
      if (string->startswith("$") && string->size() > 1) {
        VariableSpec &found = variable(scope, *string, expected);
        TermIR term;
        term.kind = TermIR::Kind::Variable;
        term.type = expected.typeIndex();
        term.variable = found.id;
        term.debug_name = string->str();
        return term;
      }
    }
    if (const Object *object = value.getAsObject()) {
      if (llvm::Optional<StringRef> name = object->getString("var")) {
        VariableSpec &found = variable(scope, *name, expected);
        TermIR term;
        term.kind = TermIR::Kind::Variable;
        term.type = expected.typeIndex();
        term.variable = found.id;
        term.debug_name = name->str();
        return term;
      }
      if (const Value *constant = object->get("const")) {
        TermIR term;
        term.kind = TermIR::Kind::Constant;
        term.type = expected.typeIndex();
        term.constant = parseConstant(*constant, expected, "constant");
        term.debug_name = "constant";
        return term;
      }
      if (allow_expression) {
        DynamicExpr expression = parseExpression(value, scope, expected);
        TermIR term;
        term.kind = TermIR::Kind::Expression;
        term.type = expected.typeIndex();
        term.expression = std::move(expression.ir);
        term.debug_name = "expression";
        return term;
      }
    }

    TermIR term;
    term.kind = TermIR::Kind::Constant;
    term.type = expected.typeIndex();
    term.constant = parseConstant(value, expected, "constant");
    term.debug_name = "constant";
    return term;
  }

  AtomIR parseAtom(const Object &atom, RuleScope &scope,
                   bool allow_expressions) {
    const RelationSpec &relation =
        findRelation(requireString(atom, "relation"));
    const Array &args = requireArray(atom, "args");
    if (args.size() != relation.columns.size())
      throw std::invalid_argument("atom arity mismatch for '" + relation.name +
                                  "'");
    AtomIR result;
    result.relation = relation.id;
    result.relation_name = relation.name;
    for (std::size_t column = 0; column < args.size(); ++column) {
      result.args.push_back(parseTerm(args[column], relation.columns[column],
                                      scope, allow_expressions));
    }
    return result;
  }

  DynamicExpr parseExpression(const Value &value, RuleScope &scope,
                              std::optional<TypeSpec> expected = std::nullopt) {
    DynamicExpr expression;
    if (llvm::Optional<StringRef> string = value.getAsString()) {
      if (string->startswith("$") && string->size() > 1)
        expression = variableExpr(findVariable(scope, *string));
      else
        expression =
            constantExpr<std::string>({ValueKind::String}, string->str());
    } else if (llvm::Optional<bool> boolean = value.getAsBoolean()) {
      expression = constantExpr<bool>({ValueKind::Bool}, *boolean);
    } else if (llvm::Optional<std::int64_t> integer = value.getAsInteger()) {
      if (expected && expected->kind == ValueKind::U64 && *integer >= 0) {
        expression = constantExpr<std::uint64_t>(
            *expected, static_cast<std::uint64_t>(*integer));
      } else if (expected && expected->kind == ValueKind::F64) {
        expression =
            constantExpr<double>(*expected, static_cast<double>(*integer));
      } else {
        expression = constantExpr<std::int64_t>({ValueKind::I64}, *integer);
      }
    } else if (llvm::Optional<double> number = value.getAsNumber()) {
      expression = constantExpr<double>({ValueKind::F64}, *number);
    } else if (const Object *object = value.getAsObject()) {
      if (llvm::Optional<StringRef> name = object->getString("var")) {
        expression = variableExpr(findVariable(scope, *name));
      } else if (const Value *constant = object->get("const")) {
        if (!expected)
          throw std::invalid_argument(
              "expression const requires an expected type");
        expression = constantAnyExpr(
            *expected, parseConstant(*constant, *expected, "expression const"));
      } else {
        expression = parseOperation(*object, scope);
      }
    } else if (expected && expected->kind == ValueKind::SetI64) {
      expression = constantAnyExpr(
          *expected, parseConstant(value, *expected, "set expression"));
    } else {
      throw std::invalid_argument("unsupported expression value");
    }

    if (expected && expression.type != *expected) {
      throw std::invalid_argument("expression type " + expression.type.name() +
                                  " does not match expected " +
                                  expected->name());
    }
    return expression;
  }

  DynamicExpr parseOperation(const Object &object, RuleScope &scope) {
    const StringRef operation = requireString(object, "op");
    const Array &args = requireArray(object, "args");
    if (operation == "!" || operation == "unary-" || operation == "unary+" ||
        operation == "min_lattice" || operation == "max_lattice" ||
        operation == "set_lattice") {
      if (args.size() != 1)
        throw std::invalid_argument("unary expression requires one argument");
      DynamicExpr operand = parseExpression(args[0], scope);
      return makeUnary(operation, std::move(operand));
    }
    if (args.size() != 2)
      throw std::invalid_argument("binary expression requires two arguments");
    DynamicExpr lhs = parseExpression(args[0], scope);
    DynamicExpr rhs = lhs.type.isLattice()
                          ? parseExpression(args[1], scope)
                          : parseExpression(args[1], scope, lhs.type);
    return makeBinary(operation, std::move(lhs), std::move(rhs));
  }

  DynamicExpr makeUnary(StringRef operation, DynamicExpr operand) {
    if (operation == "!" && operand.type.kind == ValueKind::Bool) {
      return unaryExpr<bool, bool>(
          {ValueKind::Bool}, std::move(operand),
          [](bool value) { return !value; }, "not");
    }
    if (operation == "unary-" && operand.type.kind == ValueKind::I64) {
      return unaryExpr<std::int64_t, std::int64_t>(
          {ValueKind::I64}, std::move(operand),
          [](std::int64_t value) { return -value; }, "unary-minus");
    }
    if (operation == "unary-" && operand.type.kind == ValueKind::F64) {
      return unaryExpr<double, double>(
          {ValueKind::F64}, std::move(operand),
          [](double value) { return -value; }, "unary-minus");
    }
    if (operation == "unary+")
      return operand;
    if (operation == "min_lattice" && operand.type.kind == ValueKind::I64) {
      return unaryExpr<std::int64_t, MinLattice<std::int64_t>>(
          {ValueKind::MinI64}, std::move(operand),
          [](std::int64_t value) { return MinLattice<std::int64_t>(value); },
          "min-lattice");
    }
    if (operation == "max_lattice" && operand.type.kind == ValueKind::I64) {
      return unaryExpr<std::int64_t, MaxLattice<std::int64_t>>(
          {ValueKind::MaxI64}, std::move(operand),
          [](std::int64_t value) { return MaxLattice<std::int64_t>(value); },
          "max-lattice");
    }
    if (operation == "min_lattice" && operand.type.kind == ValueKind::F64) {
      return unaryExpr<double, MinLattice<double>>(
          {ValueKind::MinF64}, std::move(operand),
          [](double value) { return MinLattice<double>(value); },
          "min-lattice");
    }
    if (operation == "max_lattice" && operand.type.kind == ValueKind::F64) {
      return unaryExpr<double, MaxLattice<double>>(
          {ValueKind::MaxF64}, std::move(operand),
          [](double value) { return MaxLattice<double>(value); },
          "max-lattice");
    }
    if (operation == "set_lattice" && operand.type.kind == ValueKind::I64) {
      return unaryExpr<std::int64_t, SetLattice<std::int64_t>>(
          {ValueKind::SetI64}, std::move(operand),
          [](std::int64_t value) { return SetLattice<std::int64_t>{value}; },
          "set-lattice");
    }
    throw std::invalid_argument("unsupported unary operation '" +
                                operation.str() + "' for " +
                                operand.type.name());
  }

  DynamicExpr makeBinary(StringRef operation, DynamicExpr lhs,
                         DynamicExpr rhs) {
    if (lhs.type != rhs.type) {
      if (operation == "+" && lhs.type.kind == ValueKind::MinI64 &&
          rhs.type.kind == ValueKind::I64) {
        return binaryExpr<MinLattice<std::int64_t>, std::int64_t,
                          MinLattice<std::int64_t>>(
            lhs.type, std::move(lhs), std::move(rhs),
            [](const auto &left, auto right) { return left + right; },
            "lattice-addition");
      }
      if (operation == "+" && lhs.type.kind == ValueKind::MaxI64 &&
          rhs.type.kind == ValueKind::I64) {
        return binaryExpr<MaxLattice<std::int64_t>, std::int64_t,
                          MaxLattice<std::int64_t>>(
            lhs.type, std::move(lhs), std::move(rhs),
            [](const auto &left, auto right) { return left + right; },
            "lattice-addition");
      }
      if (operation == "+" && lhs.type.kind == ValueKind::MinF64 &&
          rhs.type.kind == ValueKind::F64) {
        return binaryExpr<MinLattice<double>, double, MinLattice<double>>(
            lhs.type, std::move(lhs), std::move(rhs),
            [](const auto &left, double right) { return left + right; },
            "lattice-addition");
      }
      if (operation == "+" && lhs.type.kind == ValueKind::MaxF64 &&
          rhs.type.kind == ValueKind::F64) {
        return binaryExpr<MaxLattice<double>, double, MaxLattice<double>>(
            lhs.type, std::move(lhs), std::move(rhs),
            [](const auto &left, double right) { return left + right; },
            "lattice-addition");
      }
      throw std::invalid_argument("binary operands have incompatible types");
    }

    switch (lhs.type.kind) {
    case ValueKind::I64:
      return makeScalarBinary<std::int64_t>(operation, std::move(lhs),
                                            std::move(rhs));
    case ValueKind::U64:
      return makeScalarBinary<std::uint64_t>(operation, std::move(lhs),
                                             std::move(rhs));
    case ValueKind::F64:
      return makeScalarBinary<double>(operation, std::move(lhs),
                                      std::move(rhs));
    case ValueKind::String:
      return makeStringBinary(operation, std::move(lhs), std::move(rhs));
    case ValueKind::Bool:
      return makeBoolBinary(operation, std::move(lhs), std::move(rhs));
    default:
      throw std::invalid_argument("unsupported binary operands of type " +
                                  lhs.type.name());
    }
  }

  template <typename T>
  DynamicExpr makeScalarBinary(StringRef operation, DynamicExpr lhs,
                               DynamicExpr rhs) {
    const TypeSpec type = lhs.type;
    if (operation == "+")
      return sameTypeBinary<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a + b; },
          operation);
    if (operation == "-")
      return sameTypeBinary<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a - b; },
          operation);
    if (operation == "*")
      return sameTypeBinary<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a * b; },
          operation);
    if (operation == "/")
      return sameTypeBinary<T>(
          type, std::move(lhs), std::move(rhs),
          [](T a, T b) {
            if (b == T{})
              throw std::domain_error("division by zero");
            return a / b;
          },
          operation);
    if (operation == "%") {
      if constexpr (std::is_integral_v<T>) {
        return sameTypeBinary<T>(
            type, std::move(lhs), std::move(rhs),
            [](T a, T b) {
              if (b == T{})
                throw std::domain_error("remainder by zero");
              return a % b;
            },
            operation);
      }
    }
    if (operation == "==")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a == b; },
          operation);
    if (operation == "!=")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a != b; },
          operation);
    if (operation == "<")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a < b; },
          operation);
    if (operation == "<=")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a <= b; },
          operation);
    if (operation == ">")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a > b; },
          operation);
    if (operation == ">=")
      return comparison<T>(
          type, std::move(lhs), std::move(rhs), [](T a, T b) { return a >= b; },
          operation);
    throw std::invalid_argument("unsupported numeric operation '" +
                                operation.str() + "'");
  }

  DynamicExpr makeStringBinary(StringRef operation, DynamicExpr lhs,
                               DynamicExpr rhs) {
    const TypeSpec type{ValueKind::String};
    if (operation == "+")
      return sameTypeBinary<std::string>(
          type, std::move(lhs), std::move(rhs),
          [](const std::string &a, const std::string &b) { return a + b; },
          operation);
    if (operation == "==")
      return comparison<std::string>(type, std::move(lhs), std::move(rhs),
                                     std::equal_to<std::string>{}, operation);
    if (operation == "!=")
      return comparison<std::string>(type, std::move(lhs), std::move(rhs),
                                     std::not_equal_to<std::string>{},
                                     operation);
    if (operation == "<")
      return comparison<std::string>(type, std::move(lhs), std::move(rhs),
                                     std::less<std::string>{}, operation);
    throw std::invalid_argument("unsupported string operation '" +
                                operation.str() + "'");
  }

  DynamicExpr makeBoolBinary(StringRef operation, DynamicExpr lhs,
                             DynamicExpr rhs) {
    const TypeSpec type{ValueKind::Bool};
    if (operation == "&&")
      return sameTypeBinary<bool>(type, std::move(lhs), std::move(rhs),
                                  std::logical_and<bool>{}, operation);
    if (operation == "||")
      return sameTypeBinary<bool>(type, std::move(lhs), std::move(rhs),
                                  std::logical_or<bool>{}, operation);
    if (operation == "==")
      return comparison<bool>(type, std::move(lhs), std::move(rhs),
                              std::equal_to<bool>{}, operation);
    if (operation == "!=")
      return comparison<bool>(type, std::move(lhs), std::move(rhs),
                              std::not_equal_to<bool>{}, operation);
    throw std::invalid_argument("unsupported boolean operation '" +
                                operation.str() + "'");
  }

  template <typename Input, typename Output, typename State, typename MakeState,
            typename Add, typename Merge, typename Finish>
  void setReducer(AggregateIR &aggregate, MakeState make_state, Add add,
                  Merge merge, Finish finish) {
    ReducerIR reducer;
    reducer.make_state = [make_state] { return std::any(make_state()); };
    reducer.add = [add](std::any &state, const std::any &value) {
      add(std::any_cast<State &>(state), std::any_cast<const Input &>(value));
    };
    reducer.merge = [merge](std::any &state, const std::any &other) {
      merge(std::any_cast<State &>(state), std::any_cast<const State &>(other));
    };
    reducer.finish = [finish](std::any &state) {
      std::vector<Output> typed = finish(std::any_cast<State &>(state));
      std::vector<std::any> result;
      for (Output &value : typed)
        result.emplace_back(std::move(value));
      return result;
    };
    aggregate.reducer = reducer;
    aggregate.evaluate =
        [reducer = std::move(reducer)](const AggregateForEach &for_each) {
          std::any state = reducer.make_state();
          for_each([&](const std::any &value) { reducer.add(state, value); });
          return reducer.finish(state);
        };
  }

  template <typename T> struct OptionalState {
    std::optional<T> value;
  };

  struct MeanState {
    long double sum = 0;
    std::uint64_t count = 0;
  };

  TypeSpec configureAggregate(AggregateIR &aggregate, StringRef operation,
                              TypeSpec input_type) {
    if (operation == "count") {
      setReducer<std::int64_t, std::uint64_t, std::uint64_t>(
          aggregate, [] { return std::uint64_t{0}; },
          [](std::uint64_t &state, const std::int64_t &) { ++state; },
          [](std::uint64_t &state, const std::uint64_t &other) {
            state += other;
          },
          [](std::uint64_t &state) {
            return std::vector<std::uint64_t>{state};
          });
      return {ValueKind::U64};
    }
    if (input_type.kind == ValueKind::I64)
      return configureNumericAggregate<std::int64_t>(aggregate, operation,
                                                     input_type);
    if (input_type.kind == ValueKind::F64)
      return configureNumericAggregate<double>(aggregate, operation,
                                               input_type);
    throw std::invalid_argument("aggregate '" + operation.str() +
                                "' requires an i64 or f64 projection");
  }

  template <typename T>
  TypeSpec configureNumericAggregate(AggregateIR &aggregate,
                                     StringRef operation, TypeSpec input_type) {
    if (operation == "sum") {
      setReducer<T, T, T>(
          aggregate, [] { return T{}; },
          [](T &state, const T &value) { state += value; },
          [](T &state, const T &other) { state += other; },
          [](T &state) { return std::vector<T>{state}; });
      return input_type;
    }
    if (operation == "min" || operation == "max") {
      const bool minimum = operation == "min";
      setReducer<T, T, OptionalState<T>>(
          aggregate, [] { return OptionalState<T>{}; },
          [minimum](OptionalState<T> &state, const T &value) {
            if (!state.value || (minimum ? std::less<T>{}(value, *state.value)
                                         : std::less<T>{}(*state.value, value)))
              state.value = value;
          },
          [minimum](OptionalState<T> &state, const OptionalState<T> &other) {
            if (other.value &&
                (!state.value ||
                 (minimum ? std::less<T>{}(*other.value, *state.value)
                          : std::less<T>{}(*state.value, *other.value))))
              state.value = other.value;
          },
          [](OptionalState<T> &state) {
            return state.value ? std::vector<T>{*state.value}
                               : std::vector<T>{};
          });
      return input_type;
    }
    if (operation == "mean") {
      setReducer<T, double, MeanState>(
          aggregate, [] { return MeanState{}; },
          [](MeanState &state, const T &value) {
            state.sum += static_cast<long double>(value);
            ++state.count;
          },
          [](MeanState &state, const MeanState &other) {
            state.sum += other.sum;
            state.count += other.count;
          },
          [](MeanState &state) {
            if (state.count == 0)
              return std::vector<double>{};
            return std::vector<double>{
                static_cast<double>(state.sum / state.count)};
          });
      return {ValueKind::F64};
    }
    throw std::invalid_argument("unknown aggregate '" + operation.str() + "'");
  }

  AggregateIR parseAggregate(const Object &object, RuleScope &scope) {
    const StringRef operation = requireString(object, "op");
    const StringRef output_name = requireString(object, "output");
    const Object *source_object = object.getObject("source");
    if (!source_object)
      throw std::invalid_argument("aggregate requires a source atom");
    AtomIR source = parseAtom(*source_object, scope, false);

    DynamicExpr projection = constantExpr<std::int64_t>({ValueKind::I64}, 0);
    if (operation != "count") {
      const Value *value = object.get("value");
      if (!value)
        throw std::invalid_argument("aggregate requires a value expression");
      projection = parseExpression(*value, scope);
    }

    AggregateIR aggregate;
    aggregate.source = std::move(source);
    aggregate.projection = std::move(projection.ir);
    aggregate.name = operation.str();
    TypeSpec output_type =
        configureAggregate(aggregate, operation, projection.type);
    VariableSpec &output = variable(scope, output_name, output_type);
    aggregate.output_var = output.id;
    aggregate.output_type = output_type.typeIndex();
    return aggregate;
  }

  void parseRule(const Object &object, std::size_t rule_index) {
    RuleScope scope;
    scope.rule_index = rule_index;
    registerRuleVariables(object, scope);

    std::vector<BodyItemIR> body;
    for (const Value &item_value : requireArray(object, "body")) {
      const Object &item = requireObject(item_value, "body item");
      if (const Object *atom = item.getObject("atom"))
        body.push_back(parseAtom(*atom, scope, false));
      else if (const Object *negation = item.getObject("not"))
        body.push_back(NegAtomIR{parseAtom(*negation, scope, false)});
      else if (const Value *condition = item.get("where")) {
        DynamicExpr expression =
            parseExpression(*condition, scope, TypeSpec{ValueKind::Bool});
        body.push_back(FilterIR{std::move(expression.ir)});
      } else if (const Object *aggregate = item.getObject("aggregate"))
        body.push_back(parseAggregate(*aggregate, scope));
      else
        throw std::invalid_argument("unknown rule body item");
    }

    std::vector<AtomIR> heads;
    if (const Object *head = object.getObject("head"))
      heads.push_back(parseAtom(*head, scope, true));
    if (const Array *head_values = object.getArray("heads")) {
      for (const Value &head : *head_values)
        heads.push_back(parseAtom(requireObject(head, "head"), scope, true));
    }
    if (heads.empty())
      throw std::invalid_argument("rule requires head or heads");
    for (AtomIR &head : heads)
      program_.addRule({std::move(head), body});
  }

  SemanticProgram program_;
  std::vector<RelationSpec> relations_;
  std::unordered_map<std::string, RelationId> relation_ids_;
  std::vector<RelationId> outputs_;
};

} // namespace

void executeJson(StringRef input, const RunOptions &options,
                 llvm::raw_ostream &output) {
  llvm::Expected<Value> parsed = llvm::json::parse(input);
  if (!parsed)
    throw std::invalid_argument(llvm::toString(parsed.takeError()));
  const Object *root = parsed->getAsObject();
  if (!root)
    throw std::invalid_argument("JSON program root must be an object");

  JsonProgram program(*root);
  Object result = program.execute(options);
  if (options.pretty)
    output << llvm::formatv("{0:2}\n", Value(std::move(result)));
  else
    output << llvm::formatv("{0}\n", Value(std::move(result)));
}

void printSchema(llvm::raw_ostream &output) {
  output << R"json({
  "relations": [
    {
      "name": "edge",
      "columns": ["i64", "i64"],
      "facts": [[1, 2], [2, 3]]
    },
    {
      "name": "path",
      "columns": ["i64", "i64"]
    }
  ],
  "rules": [
    {
      "head": {"relation": "path", "args": ["$x", "$y"]},
      "body": [
        {"atom": {"relation": "edge", "args": ["$x", "$y"]}}
      ]
    },
    {
      "head": {"relation": "path", "args": ["$x", "$z"]},
      "body": [
        {"atom": {"relation": "path", "args": ["$x", "$y"]}},
        {"atom": {"relation": "edge", "args": ["$y", "$z"]}}
      ]
    }
  ],
  "outputs": ["path"]
})json";
}

} // namespace lotus::datalog::cli
