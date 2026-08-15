#include "Dataflow/Datalog/Internal.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace lotus::datalog::internal {
namespace {

std::string variableLabel(const VariableDefinition &variable) {
  return variable.anonymous ? "anonymous variable"
                            : "variable '" + variable.name + "'";
}

void validateTermType(const TermIR &term, const ColumnType &column,
                      const std::string &relation_name,
                      std::size_t column_index) {
  if (term.type != column.type) {
    std::ostringstream message;
    message << "type mismatch in relation '" << relation_name << "' column "
            << column_index;
    throw CompileError(message.str());
  }
}

void validateAtom(
    const AtomIR &atom,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    const std::vector<VariableDefinition> &variables,
    const std::string &position) {
  if (atom.relation >= relations.size())
    throw CompileError(position + " references an unknown relation");
  const RelationIR &relation = relations[atom.relation]->definition();
  if (atom.args.size() != relation.columns.size())
    throw CompileError("arity mismatch in relation '" + relation.name + "'");
  for (std::size_t i = 0; i < atom.args.size(); ++i) {
    const TermIR &term = atom.args[i];
    validateTermType(term, relation.columns[i], relation.name, i);
    if (term.kind == TermIR::Kind::Expression) {
      throw CompileError("expression terms are not allowed in " + position +
                         " relation '" + relation.name +
                         "'; use where(...) instead");
    }
    if (term.kind != TermIR::Kind::Variable)
      continue;
    if (term.variable >= variables.size())
      throw CompileError(position + " references an unknown variable");
    if (variables[term.variable].type != term.type)
      throw CompileError("inconsistent type for " +
                         variableLabel(variables[term.variable]));
  }
}

bool referencesGrounded(const std::vector<VarId> &references,
                        const std::vector<bool> &grounded) {
  return std::all_of(references.begin(), references.end(), [&](VarId variable) {
    return variable < grounded.size() && grounded[variable];
  });
}

std::vector<VarId> atomVariables(const AtomIR &atom) {
  std::vector<VarId> variables;
  for (const TermIR &term : atom.args) {
    if (term.kind == TermIR::Kind::Variable)
      variables.push_back(term.variable);
  }
  return variables;
}

std::size_t estimateAtomCost(
    const AtomIR &atom, const std::vector<bool> &grounded,
    const std::vector<std::unique_ptr<RelationStorage>> &relations) {
  std::size_t bound_columns = 0;
  for (const TermIR &term : atom.args) {
    if (term.kind == TermIR::Kind::Constant ||
        (term.kind == TermIR::Kind::Variable && grounded[term.variable]))
      ++bound_columns;
  }
  if (bound_columns == atom.args.size())
    return 1;
  const std::size_t row_count =
      std::max<std::size_t>(1, relations[atom.relation]->rows().size());
  const std::size_t divisor = bound_columns >= sizeof(std::size_t) * 8
                                  ? std::numeric_limits<std::size_t>::max()
                                  : (std::size_t{1} << bound_columns);
  return std::max<std::size_t>(1, row_count / divisor);
}

void markAtomGrounded(const AtomIR &atom, std::vector<bool> &grounded) {
  for (VarId variable : atomVariables(atom))
    grounded[variable] = true;
}

bool negationAvailable(const NegAtomIR &negation,
                       const std::vector<bool> &grounded,
                       const std::vector<VariableDefinition> &variables) {
  for (VarId variable : atomVariables(negation.atom)) {
    if (!variables[variable].anonymous && !grounded[variable])
      return false;
  }
  return true;
}

} // namespace

std::vector<RuleIR> planAndValidateRules(
    const std::vector<RuleIR> &input_rules,
    const std::vector<std::unique_ptr<RelationStorage>> &relations,
    const std::vector<VariableDefinition> &variables,
    std::size_t &reorder_count) {
  std::vector<RuleIR> rules;
  rules.reserve(input_rules.size());

  for (const RuleIR &input_rule : input_rules) {
    RuleIR rule = input_rule;
    std::vector<bool> grounded(variables.size(), false);
    std::vector<BodyItemIR> planned_body;
    std::size_t cursor = 0;

    while (cursor < rule.body.size()) {
      std::size_t aggregate_position = cursor;
      while (
          aggregate_position < rule.body.size() &&
          !std::holds_alternative<AggregateIR>(rule.body[aggregate_position]))
        ++aggregate_position;

      std::vector<std::size_t> pending;
      for (std::size_t i = cursor; i < aggregate_position; ++i) {
        pending.push_back(i);
        if (const auto *atom = std::get_if<AtomIR>(&rule.body[i]))
          validateAtom(*atom, relations, variables, "body atom");
        else if (const auto *negation = std::get_if<NegAtomIR>(&rule.body[i]))
          validateAtom(negation->atom, relations, variables, "negated atom");
        else {
          const FilterIR &filter = std::get<FilterIR>(rule.body[i]);
          if (filter.predicate.type != typeid(bool))
            throw CompileError("where(...) condition must have bool type");
          if (!filter.predicate.evaluate)
            throw CompileError("where(...) condition has no evaluator");
        }
      }

      while (!pending.empty()) {
        const std::size_t expected_position =
            aggregate_position - pending.size();
        auto chosen = pending.end();
        for (auto it = pending.begin(); it != pending.end(); ++it) {
          const BodyItemIR &item = rule.body[*it];
          if (const auto *filter = std::get_if<FilterIR>(&item)) {
            if (referencesGrounded(filter->predicate.referenced_vars,
                                   grounded)) {
              chosen = it;
              break;
            }
          } else if (const auto *negation = std::get_if<NegAtomIR>(&item)) {
            if (negationAvailable(*negation, grounded, variables)) {
              chosen = it;
              break;
            }
          }
        }

        if (chosen == pending.end()) {
          std::size_t best_cost = std::numeric_limits<std::size_t>::max();
          for (auto it = pending.begin(); it != pending.end(); ++it) {
            const auto *atom = std::get_if<AtomIR>(&rule.body[*it]);
            if (!atom)
              continue;
            const std::size_t cost =
                estimateAtomCost(*atom, grounded, relations);
            if (cost < best_cost) {
              best_cost = cost;
              chosen = it;
            }
          }
        }

        if (chosen == pending.end()) {
          const BodyItemIR &blocked = rule.body[pending.front()];
          if (const auto *filter = std::get_if<FilterIR>(&blocked)) {
            for (VarId variable : filter->predicate.referenced_vars) {
              if (variable >= variables.size() || !grounded[variable]) {
                const std::string label =
                    variable < variables.size()
                        ? variableLabel(variables[variable])
                        : "unknown variable";
                throw CompileError(label + " used in filter is not grounded");
              }
            }
          }
          const NegAtomIR &negation = std::get<NegAtomIR>(blocked);
          for (VarId variable : atomVariables(negation.atom)) {
            if (!variables[variable].anonymous && !grounded[variable])
              throw CompileError(variableLabel(variables[variable]) +
                                 " used in negated atom is not grounded");
          }
          throw CompileError("unable to ground rule body");
        }

        const std::size_t original_position = *chosen;
        if (original_position != expected_position)
          ++reorder_count;
        BodyItemIR selected = rule.body[original_position];
        if (const auto *atom = std::get_if<AtomIR>(&selected))
          markAtomGrounded(*atom, grounded);
        planned_body.push_back(std::move(selected));
        pending.erase(chosen);
      }

      if (aggregate_position == rule.body.size())
        break;

      const AggregateIR &aggregate =
          std::get<AggregateIR>(rule.body[aggregate_position]);
      validateAtom(aggregate.source, relations, variables,
                   "aggregate source atom");
      if (aggregate.output_var >= variables.size())
        throw CompileError("aggregate references an unknown output variable");
      if (variables[aggregate.output_var].anonymous)
        throw CompileError("aggregate output may not be anonymous");
      if (variables[aggregate.output_var].type != aggregate.output_type)
        throw CompileError("aggregate output variable has inconsistent type");
      if (grounded[aggregate.output_var])
        throw CompileError("aggregate output variable is already grounded");
      if (!aggregate.evaluate_range)
        throw CompileError("aggregate '" + aggregate.name +
                           "' has no evaluator");

      std::vector<bool> aggregate_grounded = grounded;
      for (VarId variable : atomVariables(aggregate.source)) {
        if (variable == aggregate.output_var)
          throw CompileError(
              "aggregate output variable may not appear in its source atom");
        aggregate_grounded[variable] = true;
      }
      if (!referencesGrounded(aggregate.projection.referenced_vars,
                              aggregate_grounded)) {
        throw CompileError("aggregate projection references an ungrounded "
                           "variable");
      }
      planned_body.push_back(aggregate);
      grounded[aggregate.output_var] = true;
      cursor = aggregate_position + 1;
    }

    rule.body = std::move(planned_body);
    if (rule.head.relation >= relations.size())
      throw CompileError("rule head references an unknown relation");
    const RelationIR &head_relation =
        relations[rule.head.relation]->definition();
    if (rule.head.args.size() != head_relation.columns.size())
      throw CompileError("arity mismatch in relation '" + head_relation.name +
                         "'");
    for (std::size_t i = 0; i < rule.head.args.size(); ++i) {
      const TermIR &term = rule.head.args[i];
      validateTermType(term, head_relation.columns[i], head_relation.name, i);
      if (term.kind == TermIR::Kind::Variable) {
        if (term.variable >= variables.size() || !grounded[term.variable]) {
          const std::string label =
              term.variable < variables.size()
                  ? variableLabel(variables[term.variable])
                  : "unknown variable";
          throw CompileError(label + " used in head is not grounded");
        }
      } else if (term.kind == TermIR::Kind::Expression) {
        if (!term.expression.evaluate)
          throw CompileError("head expression has no evaluator");
        if (!referencesGrounded(term.expression.referenced_vars, grounded))
          throw CompileError(
              "head expression references an ungrounded variable");
      }
    }
    rules.push_back(std::move(rule));
  }
  return rules;
}

} // namespace lotus::datalog::internal
