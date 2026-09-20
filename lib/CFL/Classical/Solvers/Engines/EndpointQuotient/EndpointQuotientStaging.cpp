#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotientStaging.h"

#include "Utils/ADT/TarjanScc.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace lotus::cfl::endpoint {
namespace {

void sortUnique(std::vector<Id> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

Rule normalizedRule(Rule rule) {
  if (rule.kind == Rule::Kind::Epsilon) {
    rule.left = 0;
    rule.right = 0;
  } else if (rule.kind == Rule::Kind::Unary) {
    rule.right = 0;
  }
  return rule;
}

auto ruleKey(const Rule &rule) {
  return std::make_tuple(static_cast<unsigned>(rule.kind), rule.lhs, rule.left,
                         rule.right);
}

std::vector<Rule> canonicalRules(const std::vector<Rule> &rules) {
  std::vector<Rule> result;
  result.reserve(rules.size());
  for (Rule rule : rules)
    result.push_back(normalizedRule(rule));
  std::sort(result.begin(), result.end(), [](const Rule &lhs, const Rule &rhs) {
    return ruleKey(lhs) < ruleKey(rhs);
  });
  result.erase(std::unique(result.begin(), result.end(),
                           [](const Rule &lhs, const Rule &rhs) {
                             return ruleKey(lhs) == ruleKey(rhs);
                           }),
               result.end());
  return result;
}

template <typename Visitor>
void forEachRhs(const Rule &rule, const Visitor &visitor) {
  if (rule.kind == Rule::Kind::Unary || rule.kind == Rule::Kind::Binary)
    visitor(rule.left);
  if (rule.kind == Rule::Kind::Binary)
    visitor(rule.right);
}

bool mentionsComponent(const Rule &rule, Id raw_component,
                       const std::vector<Id> &raw_component_of) {
  bool result = false;
  forEachRhs(rule, [&](Id symbol) {
    result = result || raw_component_of[symbol] == raw_component;
  });
  return result;
}

SccKind classify(const StagingStage &stage) {
  if (stage.recursive_rules.empty())
    return SccKind::Acyclic;

  bool unary_only = true;
  for (const Rule &rule : stage.recursive_rules)
    unary_only = unary_only && rule.kind == Rule::Kind::Unary;
  if (unary_only)
    return SccKind::UnaryRegular;

  if (stage.symbols.size() == 1) {
    const Id symbol = stage.symbols.front();
    bool has_transitive_rule = false;
    bool transitive_self = true;
    bool has_linear_rule = false;
    bool left_linear = true;
    bool right_linear = true;
    for (const Rule &rule : stage.recursive_rules) {
      if (rule.kind == Rule::Kind::Unary) {
        transitive_self = transitive_self && rule.left == symbol;
        left_linear = left_linear && rule.left == symbol;
        right_linear = right_linear && rule.left == symbol;
      } else if (rule.kind == Rule::Kind::Binary) {
        const bool is_transitive = rule.left == symbol && rule.right == symbol;
        has_transitive_rule = has_transitive_rule || is_transitive;
        transitive_self = transitive_self && is_transitive;
        const bool is_left_linear = rule.left == symbol && rule.right != symbol;
        const bool is_right_linear =
            rule.left != symbol && rule.right == symbol;
        has_linear_rule = has_linear_rule || is_left_linear || is_right_linear;
        left_linear = left_linear && is_left_linear;
        right_linear = right_linear && is_right_linear;
      } else {
        transitive_self = false;
        left_linear = false;
        right_linear = false;
      }
    }
    if (transitive_self && has_transitive_rule)
      return SccKind::TransitiveSelf;
    if (has_linear_rule && left_linear)
      return SccKind::LeftLinear;
    if (has_linear_rule && right_linear)
      return SccKind::RightLinear;
  }

  return SccKind::General;
}

} // namespace

const StagingStage &StagingPlan::stageForSymbol(Id symbol) const {
  if (symbol >= symbol_to_stage.size())
    throw std::out_of_range("endpoint quotient staging symbol out of range");
  return stages[symbol_to_stage[symbol]];
}

StagingPlan buildStagingPlan(const Problem &problem) {
  problem.validate();

  StagingPlan plan;
  plan.symbol_to_stage.resize(problem.symbols);
  if (problem.symbols == 0)
    return plan;

  const std::vector<Rule> rules = canonicalRules(problem.rules);
  std::vector<std::vector<Id>> successors(problem.symbols);
  for (const Rule &rule : rules)
    forEachRhs(rule,
               [&](Id symbol) { successors[rule.lhs].push_back(symbol); });
  for (auto &targets : successors)
    sortUnique(targets);

  auto successor_range = [&](Id symbol) -> const std::vector<Id> & {
    return successors[symbol];
  };
  std::vector<Id> raw_component_of;
  std::vector<Id> reverse_topological_order;
  const Id component_count = FindStronglyConnectedComponents(
      problem.symbols, successor_range, raw_component_of,
      reverse_topological_order);

  std::vector<std::vector<Id>> members(component_count);
  for (Id symbol = 0; symbol < problem.symbols; ++symbol)
    members[raw_component_of[symbol]].push_back(symbol);

  // Condensation edges point dependency -> dependent so Kahn's order is the
  // order in which a staged evaluator can consume already-computed relations.
  std::vector<std::vector<Id>> raw_dependencies(component_count);
  std::vector<std::vector<Id>> raw_dependents(component_count);
  for (Id lhs = 0; lhs < problem.symbols; ++lhs) {
    const Id dependent = raw_component_of[lhs];
    for (Id rhs : successors[lhs]) {
      const Id dependency = raw_component_of[rhs];
      if (dependency == dependent)
        continue;
      raw_dependencies[dependent].push_back(dependency);
      raw_dependents[dependency].push_back(dependent);
    }
  }
  for (Id component = 0; component < component_count; ++component) {
    sortUnique(raw_dependencies[component]);
    sortUnique(raw_dependents[component]);
  }

  using QueueEntry = std::pair<Id, Id>; // minimum symbol, raw component
  std::priority_queue<QueueEntry, std::vector<QueueEntry>,
                      std::greater<QueueEntry>>
      ready;
  std::vector<Id> remaining_dependencies(component_count);
  for (Id component = 0; component < component_count; ++component) {
    remaining_dependencies[component] = raw_dependencies[component].size();
    if (remaining_dependencies[component] == 0)
      ready.emplace(members[component].front(), component);
  }

  std::vector<Id> raw_in_plan_order;
  raw_in_plan_order.reserve(component_count);
  while (!ready.empty()) {
    const Id component = ready.top().second;
    ready.pop();
    raw_in_plan_order.push_back(component);
    for (Id dependent : raw_dependents[component]) {
      if (--remaining_dependencies[dependent] == 0)
        ready.emplace(members[dependent].front(), dependent);
    }
  }
  if (raw_in_plan_order.size() != component_count)
    throw std::logic_error("endpoint quotient SCC condensation is cyclic");

  std::vector<Id> raw_to_stage(component_count);
  plan.stages.resize(component_count);
  for (Id stage_index = 0; stage_index < component_count; ++stage_index) {
    const Id raw_component = raw_in_plan_order[stage_index];
    raw_to_stage[raw_component] = stage_index;
    StagingStage &stage = plan.stages[stage_index];
    stage.index = stage_index;
    stage.symbols = members[raw_component];
    for (Id symbol : stage.symbols)
      plan.symbol_to_stage[symbol] = stage_index;
  }

  for (const Rule &rule : rules) {
    const Id raw_component = raw_component_of[rule.lhs];
    StagingStage &stage = plan.stages[raw_to_stage[raw_component]];
    stage.rules.push_back(rule);
    if (mentionsComponent(rule, raw_component, raw_component_of))
      stage.recursive_rules.push_back(rule);
  }

  for (Id stage_index = 0; stage_index < component_count; ++stage_index) {
    const Id raw_component = raw_in_plan_order[stage_index];
    StagingStage &stage = plan.stages[stage_index];
    for (Id dependency : raw_dependencies[raw_component])
      stage.dependencies.push_back(raw_to_stage[dependency]);
    for (Id dependent : raw_dependents[raw_component])
      stage.dependents.push_back(raw_to_stage[dependent]);
    sortUnique(stage.dependencies);
    sortUnique(stage.dependents);
    stage.kind = classify(stage);
  }

  return plan;
}

} // namespace lotus::cfl::endpoint
