#include "Dataflow/WPDS/Backend/Model.h"

#include <cassert>

namespace wpds::backend {

Model::Model() : stackSymbols{"<epsilon>"} {}

ControlStateId Model::addControlState(const std::string &name) {
  controlStates.push_back(name);
  return static_cast<ControlStateId>(controlStates.size() - 1);
}

StackSymbolId Model::addStackSymbol(const std::string &name) {
  stackSymbols.push_back(name);
  return static_cast<StackSymbolId>(stackSymbols.size() - 1);
}

void Model::addPopRule(ControlStateId fromState, StackSymbolId fromStack,
                       ControlStateId toState, const GenKillValue &weight,
                       const std::string &origin) {
  modelRules.push_back({RuleKind::Pop, fromState, fromStack, toState, Epsilon,
                        Epsilon, weight, origin});
}

void Model::addReplaceRule(ControlStateId fromState, StackSymbolId fromStack,
                           ControlStateId toState, StackSymbolId toStack,
                           const GenKillValue &weight,
                           const std::string &origin) {
  modelRules.push_back({RuleKind::Replace, fromState, fromStack, toState,
                        toStack, Epsilon, weight, origin});
}

void Model::addPushRule(ControlStateId fromState, StackSymbolId fromStack,
                        ControlStateId toState, StackSymbolId toStack1,
                        StackSymbolId toStack2, const GenKillValue &weight,
                        const std::string &origin) {
  modelRules.push_back({RuleKind::Push, fromState, fromStack, toState, toStack1,
                        toStack2, weight, origin});
}

const std::string &Model::controlStateName(ControlStateId id) const {
  assert(id < controlStates.size());
  return controlStates[id];
}

const std::string &Model::stackSymbolName(StackSymbolId id) const {
  assert(id < stackSymbols.size());
  return stackSymbols[id];
}

const std::vector<std::string> &Model::controlStateNames() const {
  return controlStates;
}

const std::vector<std::string> &Model::stackSymbolNames() const {
  return stackSymbols;
}

const std::vector<Rule> &Model::rules() const { return modelRules; }

void Model::addProcedureEntry(StackSymbolId entry) { entries.insert(entry); }

const std::set<StackSymbolId> &Model::procedureEntries() const {
  return entries;
}

} // namespace wpds::backend
