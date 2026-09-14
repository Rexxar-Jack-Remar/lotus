#ifndef LOTUS_LIB_DATAFLOW_WPDS_BACKEND_MODEL_H_
#define LOTUS_LIB_DATAFLOW_WPDS_BACKEND_MODEL_H_

#include "Dataflow/WPDS/Backend.h"
#include "Dataflow/WPDS/Core/GenKillValue.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wpds::backend {

using ControlStateId = std::uint32_t;
using StackSymbolId = std::uint32_t;
constexpr StackSymbolId Epsilon = 0;

enum class RuleKind { Pop, Replace, Push };

struct Rule {
  RuleKind kind = RuleKind::Pop;
  ControlStateId fromState = 0;
  StackSymbolId fromStack = Epsilon;
  ControlStateId toState = 0;
  StackSymbolId toStack1 = Epsilon;
  StackSymbolId toStack2 = Epsilon;
  GenKillValue weight = GenKillValue::one();
  std::string origin;
};

class Model {
public:
  Model();

  ControlStateId addControlState(const std::string &name);
  StackSymbolId addStackSymbol(const std::string &name);
  void addPopRule(ControlStateId fromState, StackSymbolId fromStack,
                  ControlStateId toState, const GenKillValue &weight,
                  const std::string &origin);
  void addReplaceRule(ControlStateId fromState, StackSymbolId fromStack,
                      ControlStateId toState, StackSymbolId toStack,
                      const GenKillValue &weight, const std::string &origin);
  void addPushRule(ControlStateId fromState, StackSymbolId fromStack,
                   ControlStateId toState, StackSymbolId toStack1,
                   StackSymbolId toStack2, const GenKillValue &weight,
                   const std::string &origin);

  const std::string &controlStateName(ControlStateId id) const;
  const std::string &stackSymbolName(StackSymbolId id) const;
  const std::vector<std::string> &controlStateNames() const;
  const std::vector<std::string> &stackSymbolNames() const;
  const std::vector<Rule> &rules() const;

  void addProcedureEntry(StackSymbolId entry);
  const std::set<StackSymbolId> &procedureEntries() const;

private:
  std::vector<std::string> controlStates;
  std::vector<std::string> stackSymbols;
  std::vector<Rule> modelRules;
  std::set<StackSymbolId> entries;
};

struct Query {
  WPDSQueryKind operation = WPDSQueryKind::PostStar;
  WPDSObservationKind observation = WPDSObservationKind::ExactStack;
  ControlStateId initialState = 0;
  std::vector<StackSymbolId> roots;
  GenKillValue seed = GenKillValue::one();
};

struct Observation {
  bool reachable = false;
  GenKillValue summary = GenKillValue::zero();
};

struct QueryResult {
  std::map<StackSymbolId, Observation> observations;
};

} // namespace wpds::backend

#endif // LOTUS_LIB_DATAFLOW_WPDS_BACKEND_MODEL_H_
